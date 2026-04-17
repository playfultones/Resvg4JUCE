/*
This file is part of Resvg4JUCE.

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at http://mozilla.org/MPL/2.0/.

*/

namespace jb
{

/** A simple two state button using two SVGs for on and off state */
class SVGButton : public juce::Button
{
public:

    SVGButton (const char* offData, int offSize, const char* onData, int onSize, const juce::String& buttonName = "")
      : juce::Button (buttonName)
    {
        auto successLoadingOff = offSVG.loadFromBinaryData (offData, offSize);
        auto successLoadingOn  = onSVG.loadFromBinaryData (onData, onSize);

        jassert (successLoadingOff);
        jassert (successLoadingOn);

        juce::ignoreUnused (successLoadingOff, successLoadingOn);
    }

    void resized() override
    {
        renderIfNeeded();
    }

private:
    Resvg::RenderTree offSVG;
    Resvg::RenderTree onSVG;

    juce::Image offImage;
    juce::Image onImage;

    juce::Colour backgroundColour = juce::Colours::transparentBlack;

    juce::Rectangle<float> cachedImageBounds;

    void renderIfNeeded()
    {
        if (getWidth() <= 0 || getHeight() <= 0)
            return;

        auto componentScale = getApproximateScaleFactorForComponent (this);

        double displayScale = 1.0;
        if (auto* peer = getPeer())
            displayScale = peer->getPlatformScaleFactor();
        else if (auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForPoint (
                     localPointToGlobal (getLocalBounds().getCentre())))
            displayScale = display->scale;

        auto newImageBounds = getLocalBounds().toFloat() * (float) (displayScale * componentScale);

        if (newImageBounds == cachedImageBounds)
            return;

        offImage = offSVG.render (newImageBounds, backgroundColour);
        onImage  = onSVG.render  (newImageBounds, backgroundColour);

        cachedImageBounds = newImageBounds;
    }

    void paintButton (juce::Graphics& g, bool, bool) override
    {
        renderIfNeeded();

        auto& imageToDraw = getToggleState() ? onImage : offImage;

        g.drawImage (imageToDraw, getLocalBounds().toFloat(), juce::RectanglePlacement::centred);
    }
};
}
