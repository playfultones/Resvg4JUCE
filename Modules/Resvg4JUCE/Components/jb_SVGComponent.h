/*
This file is part of Resvg4JUCE.

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at http://mozilla.org/MPL/2.0/.

*/

namespace jb
{

    /**
 * A component that owns an Resvg::RenderTree. On each resize, it renders an Image according to the Components size
 * and displays it according to the placement set through setImagePlacement (default is centred)
 */
    class SVGComponent : public juce::Component
    {
    public:
        /**
     * Creates an SVGComponent from an svg file. You have to make sure that this is a valid svg, otherwise
     * behaviour is undefined
     */
        SVGComponent (const juce::File& svgFile)
        {
            auto successLoading = svg.loadFromFile (svgFile);

            jassert (successLoading);
            juce::ignoreUnused (successLoading);
        }

        /**
     * Creates an SVGComponent from an binary data. You have to make sure that this is a valid svg, otherwise
     * behaviour is undefined
     */
        SVGComponent (const char* svgData, int svgSize)
        {
            auto successLoading = svg.loadFromBinaryData (svgData, svgSize);
            contentHash = computeContentHash (svgData, svgSize);

            jassert (successLoading);
            juce::ignoreUnused (successLoading);
        }

        /** Creates an SVGComponent from a pre-generated svgRenderTree */
        SVGComponent (Resvg::RenderTree&& svgRenderTree) : svg (std::move (svgRenderTree))
        {
            jassert (svg.isValid());
        }

        /** Sets how the image generated from the SVG is placed on the components surface */
        void setImagePlacement (juce::RectanglePlacement placement)
        {
            imagePlacement = placement;
        }

        /** Returns how the image generated from the SVG is placed on the components surface */
        juce::RectanglePlacement getImagePlacement()
        {
            return imagePlacement;
        }

        /** Provide the binary data and size for shared image caching.
         *  A content-based hash of the SVG data is computed once and used as part
         *  of the juce::ImageCache key (combined with rendered pixel dimensions).
         *  Multiple SVGComponent instances rendering the same SVG at the same size
         *  will share a single rasterized juce::Image. */
        void setCacheIdentity (const char* data, int size)
        {
            contentHash = computeContentHash (data, size);
        }

        void resized() override
        {
            renderIfNeeded();
        }

        void paint (juce::Graphics& g) override
        {
            // Re-render if the effective scale changed since last render
            // (e.g. the parent's AffineTransform changed without triggering resized)
            renderIfNeeded();

            g.drawImage (cachedImage, getLocalBounds().toFloat(), imagePlacement);
        }

    private:
        SVGComponent() {}

        void renderIfNeeded()
        {
            if (getWidth() <= 0 || getHeight() <= 0)
                return;

            auto componentScale = getApproximateScaleFactorForComponent (this);

            // Use the display scale for the screen this component is actually on
            double displayScale = 1.0;
            if (auto* peer = getPeer())
                displayScale = peer->getPlatformScaleFactor();
            else if (auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForPoint (
                         localPointToGlobal (getLocalBounds().getCentre())))
                displayScale = display->scale;

            auto newImageBounds = getLocalBounds().toFloat() * (float) (displayScale * componentScale);

            if (newImageBounds == cachedImageBounds)
                return;

            auto pixelW = (int) std::ceil (newImageBounds.getWidth());
            auto pixelH = (int) std::ceil (newImageBounds.getHeight());

            if (contentHash != 0 && pixelW > 0 && pixelH > 0)
            {
                auto hashCode = makeImageCacheKey (pixelW, pixelH);
                auto cached = juce::ImageCache::getFromHashCode (hashCode);

                if (cached.isValid())
                {
                    cachedImage = cached;
                    cachedImageBounds = newImageBounds;
                    return;
                }

                cachedImage = svg.render (newImageBounds);
                cachedImageBounds = newImageBounds;
                juce::ImageCache::addImageToCache (cachedImage, hashCode);
                return;
            }

            cachedImage = svg.render (newImageBounds);
            cachedImageBounds = newImageBounds;
        }

        /** Hash the actual SVG content bytes — deterministic regardless of memory layout. */
        static int64_t computeContentHash (const char* data, int size)
        {
            // FNV-1a 64-bit over the full content
            uint64_t h = 14695981039346656037ULL;
            for (int i = 0; i < size; ++i)
            {
                h ^= static_cast<uint64_t> (static_cast<unsigned char> (data[i]));
                h *= 1099511628211ULL;
            }
            return static_cast<int64_t> (h);
        }

        int64_t makeImageCacheKey (int pixelW, int pixelH) const
        {
            // Combine content hash + rendered pixel dimensions
            auto h = contentHash;
            h ^= static_cast<int64_t> (pixelW) * 0x517cc1b727220a95LL;
            h ^= static_cast<int64_t> (pixelH) * 0x6c62272e07bb0142LL;
            return h;
        }

        Resvg::RenderTree svg;

        int64_t contentHash = 0;

        juce::Image cachedImage;
        juce::Rectangle<float> cachedImageBounds;

        juce::RectanglePlacement imagePlacement = juce::RectanglePlacement::centred;
    };

}
