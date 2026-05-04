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

        /** Supersample factor over physical pixels. With the parent component
         *  applying a scale transform (e.g. mainComponent.setTransform(scale(uiSize))),
         *  we want the SVG bitmap to have enough source detail that the inevitable
         *  downsample to physical pixels stays crisp. 2x is a sweet spot — costs
         *  4x bitmap memory per icon (icons are small) and gives Lanczos / bilinear
         *  enough headroom to avoid the muddy look of sampling near-identity ratios. */
        static constexpr float supersample = 2.0f;

        void resized() override
        {
            auto displayScale = juce::Desktop::getInstance().getDisplays().getDisplayForPoint (getScreenBounds().getCentre())->scale;
            auto componentScale = getApproximateScaleFactorForComponent (this);
            auto totalScale = static_cast<float> (displayScale) * componentScale;
            auto renderScale = totalScale * supersample;

            auto newImageBounds = getLocalBounds().toFloat() * renderScale;

            if (newImageBounds == cachedImageBounds && renderScale == cachedRenderScale)
                return;

            auto pixelW = (int) std::ceil (newImageBounds.getWidth());
            auto pixelH = (int) std::ceil (newImageBounds.getHeight());

            cachedRenderScale = renderScale;

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

        void paint (juce::Graphics& g) override
        {
            if (! cachedImage.isValid() || cachedRenderScale <= 0.0f)
                return;

            // Image is at supersample × physical resolution. Draw it with an explicit
            // transform so its pixels map cleanly onto physical pixels through the
            // parent component's scale — the composed transform is exactly
            // `1/supersample`, which is a clean high-ratio downsample (single bilinear
            // pass at 0.5 ratio) instead of two near-identity passes.
            g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);

            const auto imgW = (float) cachedImage.getWidth();
            const auto imgH = (float) cachedImage.getHeight();

            const juce::Rectangle<float> imgInLocal (imgW / cachedRenderScale,
                                                     imgH / cachedRenderScale);
            const auto placedLocal = imagePlacement.appliedTo (imgInLocal, getLocalBounds().toFloat());

            const auto t = juce::AffineTransform::scale (placedLocal.getWidth() / imgW,
                                                          placedLocal.getHeight() / imgH)
                               .translated (placedLocal.getX(), placedLocal.getY());

            g.drawImageTransformed (cachedImage, t);
        }

    private:
        SVGComponent() {}

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
        float cachedRenderScale = 0.0f;

        juce::RectanglePlacement imagePlacement = juce::RectanglePlacement::centred;
    };

}
