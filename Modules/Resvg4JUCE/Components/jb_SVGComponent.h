/*
This file is part of Resvg4JUCE.

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at http://mozilla.org/MPL/2.0/.

*/

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace jb
{

    /** Shared thread pool for asynchronous SVG rasterisation. Process-wide
     *  Meyers singleton so editor open/close cycles don't wait on
     *  `juce::ThreadPool::removeAllJobs`. */
    inline juce::ThreadPool& getResvgRenderPool()
    {
        static juce::ThreadPool pool { juce::ThreadPoolOptions {}
                                           .withThreadName ("Resvg Render Pool")
                                           .withNumberOfThreads (juce::jmax (2, juce::SystemStats::getNumCpus() / 2)) };
        return pool;
    }

    /** FNV-1a 64-bit content hash — deterministic, used as part of juce::ImageCache keys. */
    inline int64_t computeSvgContentHash (const char* data, int size)
    {
        uint64_t h = 14695981039346656037ULL;
        for (int i = 0; i < size; ++i)
        {
            h ^= static_cast<uint64_t> (static_cast<unsigned char> (data[i]));
            h *= 1099511628211ULL;
        }
        return static_cast<int64_t> (h);
    }

    inline int64_t makeSvgImageCacheKey (int64_t contentHash, int pixelW, int pixelH)
    {
        auto h = contentHash;
        h ^= static_cast<int64_t> (pixelW) * 0x517cc1b727220a95LL;
        h ^= static_cast<int64_t> (pixelH) * 0x6c62272e07bb0142LL;
        return h;
    }

    /** Per-tree render state shared between an SVGComponent and its in-flight
     *  background jobs via shared_ptr — keeps the tree alive across component
     *  destruction, and serialises render calls between the message thread
     *  and worker pool. */
    struct AsyncSvgRenderState
    {
        Resvg::RenderTree tree;
        std::mutex renderMutex;
        std::atomic<int> generation { 0 };

        AsyncSvgRenderState() = default;
        AsyncSvgRenderState (Resvg::RenderTree&& movedTree) : tree (std::move (movedTree)) {}
    };

    /**
 * A component that owns an Resvg::RenderTree. On each resize, it renders an Image
 * according to the Components size and displays it according to the placement set
 * through setImagePlacement (default is centred).
 *
 * Rendering is two-phase: a low-resolution placeholder is rasterised synchronously
 * (skipped if a worker is already mid-render), then a supersampled image is
 * rendered on a shared background thread pool and swapped in once ready. Stale
 * results from superseded resizes are dropped via a generation counter; cached
 * images for matching pixel dimensions are reused via juce::ImageCache.
 */
    class SVGComponent : public juce::Component
    {
    public:
        /**
     * Creates an SVGComponent from an svg file. You have to make sure that this is a valid svg, otherwise
     * behaviour is undefined
     */
        SVGComponent (const juce::File& svgFile)
            : state (std::make_shared<AsyncSvgRenderState>())
        {
            auto successLoading = state->tree.loadFromFile (svgFile);

            jassert (successLoading);
            juce::ignoreUnused (successLoading);
        }

        /**
     * Creates an SVGComponent from an binary data. You have to make sure that this is a valid svg, otherwise
     * behaviour is undefined
     */
        SVGComponent (const char* svgData, int svgSize)
            : state (std::make_shared<AsyncSvgRenderState>())
        {
            auto successLoading = state->tree.loadFromBinaryData (svgData, svgSize);
            contentHash = computeSvgContentHash (svgData, svgSize);

            jassert (successLoading);
            juce::ignoreUnused (successLoading);
        }

        /** Creates an SVGComponent from a pre-generated svgRenderTree */
        SVGComponent (Resvg::RenderTree&& svgRenderTree)
            : state (std::make_shared<AsyncSvgRenderState> (std::move (svgRenderTree)))
        {
            jassert (state->tree.isValid());
        }

        ~SVGComponent() override
        {
            // Bumping the shared generation cancels every in-flight job that
            // belongs to us. The worker reads `state->generation` (not us), so
            // this is safe even after our members start unwinding.
            if (state != nullptr)
                state->generation.fetch_add (1, std::memory_order_relaxed);

            masterReference.clear();
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
            contentHash = computeSvgContentHash (data, size);
        }

        /** Default supersample factor over physical pixels. */
        static constexpr float defaultSupersample = 2.0f;

        /** Override the supersample factor for this instance. Larger values
         *  trade quadratically more bitmap memory for sharper output. */
        void setSupersampleFactor (float newFactor)
        {
            jassert (newFactor > 0.0f);
            if (juce::approximatelyEqual (newFactor, supersample))
                return;

            supersample = newFactor;

            if (state != nullptr)
                state->generation.fetch_add (1, std::memory_order_relaxed);

            cachedImage = juce::Image();
            cachedRenderScale = 0.0f;
            cachedImageBounds = {};

            if (! getLocalBounds().isEmpty())
                resized();

            repaint();
        }

        float getSupersampleFactor() const noexcept { return supersample; }

        void resized() override
        {
            if (state == nullptr)
                return;

            const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForPoint (getScreenBounds().getCentre());
            const float displayScale  = display != nullptr ? static_cast<float> (display->scale) : 1.0f;
            const float componentScale = getApproximateScaleFactorForComponent (this);
            const float totalScale     = displayScale * componentScale;
            const float highResScale   = totalScale * supersample;

            const auto localF        = getLocalBounds().toFloat();
            const auto highResBounds = localF * highResScale;
            const auto lowResBounds  = localF * totalScale;

            const int hiW = (int) std::ceil (highResBounds.getWidth());
            const int hiH = (int) std::ceil (highResBounds.getHeight());

            if (cachedImage.isValid()
                && juce::approximatelyEqual (cachedRenderScale, highResScale)
                && cachedImage.getWidth()  == hiW
                && cachedImage.getHeight() == hiH)
                return;

            const int myGen = state->generation.fetch_add (1, std::memory_order_relaxed) + 1;

            if (contentHash != 0 && hiW > 0 && hiH > 0)
            {
                const auto hashCode = makeSvgImageCacheKey (contentHash, hiW, hiH);
                if (auto cached = juce::ImageCache::getFromHashCode (hashCode); cached.isValid())
                {
                    cachedImage       = std::move (cached);
                    cachedRenderScale = highResScale;
                    cachedImageBounds = highResBounds;
                    return;
                }
            }

            // Synchronous low-res placeholder — try_lock so we never block the
            // message thread on an in-flight worker. If contended, the async
            // high-res path below covers it.
            const int loW = (int) std::ceil (lowResBounds.getWidth());
            const int loH = (int) std::ceil (lowResBounds.getHeight());

            if (loW > 0 && loH > 0)
            {
                juce::Image initial;
                const int64_t loHash = (contentHash != 0) ? makeSvgImageCacheKey (contentHash, loW, loH) : 0;

                if (loHash != 0)
                    initial = juce::ImageCache::getFromHashCode (loHash);

                if (! initial.isValid())
                {
                    if (state->renderMutex.try_lock())
                    {
                        std::lock_guard<std::mutex> lock (state->renderMutex, std::adopt_lock);
                        initial = state->tree.render (lowResBounds);
                        if (loHash != 0)
                            juce::ImageCache::addImageToCache (initial, loHash);
                    }
                }

                if (initial.isValid())
                {
                    cachedImage       = std::move (initial);
                    cachedRenderScale = totalScale;
                    cachedImageBounds = lowResBounds;
                }
            }

            if (hiW > 0 && hiH > 0)
            {
                const int64_t hiHashCode = (contentHash != 0) ? makeSvgImageCacheKey (contentHash, hiW, hiH) : 0;
                getResvgRenderPool().addJob (new HighResRenderJob (state,
                                                                    highResBounds,
                                                                    highResScale,
                                                                    hiHashCode,
                                                                    juce::WeakReference<SVGComponent> (this),
                                                                    myGen),
                                              true);
            }
        }

        void paint (juce::Graphics& g) override
        {
            if (! cachedImage.isValid() || cachedRenderScale <= 0.0f)
                return;

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

        /** Called on the message thread when an async high-res render lands. */
        void publishHighRes (juce::Image image, float scale, juce::Rectangle<float> bounds)
        {
            cachedImage       = std::move (image);
            cachedRenderScale = scale;
            cachedImageBounds = bounds;
            repaint();
        }

        /** Background job that rasterises the SVG at the supersampled resolution. */
        class HighResRenderJob : public juce::ThreadPoolJob
        {
        public:
            HighResRenderJob (std::shared_ptr<AsyncSvgRenderState> sharedState,
                              juce::Rectangle<float> renderBounds,
                              float renderScale,
                              int64_t cacheKeyValue,
                              juce::WeakReference<SVGComponent> ownerWeak,
                              int generation)
                : juce::ThreadPoolJob ("Resvg high-res render"),
                  state (std::move (sharedState)),
                  bounds (renderBounds),
                  scale (renderScale),
                  cacheKey (cacheKeyValue),
                  weak (std::move (ownerWeak)),
                  gen (generation)
            {
            }

            JobStatus runJob() override
            {
                if (shouldExit() || state == nullptr)
                    return jobHasFinished;

                if (state->generation.load (std::memory_order_relaxed) != gen)
                    return jobHasFinished;

                juce::Image rendered;
                if (cacheKey != 0)
                    rendered = juce::ImageCache::getFromHashCode (cacheKey);

                if (! rendered.isValid())
                {
                    std::lock_guard<std::mutex> lock (state->renderMutex);

                    if (state->generation.load (std::memory_order_relaxed) != gen)
                        return jobHasFinished;

                    if (cacheKey != 0)
                        rendered = juce::ImageCache::getFromHashCode (cacheKey);

                    if (! rendered.isValid())
                    {
                        rendered = state->tree.render (bounds);
                        if (cacheKey != 0)
                            juce::ImageCache::addImageToCache (rendered, cacheKey);
                    }
                }

                if (shouldExit())
                    return jobHasFinished;

                auto weakCopy   = weak;
                auto genCopy    = gen;
                auto stateCopy  = state;
                auto scaleCopy  = scale;
                auto boundsCopy = bounds;
                juce::MessageManager::callAsync ([weakCopy, genCopy, stateCopy, rendered = std::move (rendered), boundsCopy, scaleCopy]() mutable
                {
                    if (auto* owner = weakCopy.get())
                        if (stateCopy->generation.load (std::memory_order_relaxed) == genCopy)
                            owner->publishHighRes (std::move (rendered), scaleCopy, boundsCopy);
                });

                return jobHasFinished;
            }

        private:
            std::shared_ptr<AsyncSvgRenderState> state;
            juce::Rectangle<float> bounds;
            float scale;
            int64_t cacheKey;
            juce::WeakReference<SVGComponent> weak;
            int gen;
        };

        std::shared_ptr<AsyncSvgRenderState> state;

        float supersample = defaultSupersample;

        int64_t contentHash = 0;

        juce::Image cachedImage;
        juce::Rectangle<float> cachedImageBounds;
        float cachedRenderScale = 0.0f;

        juce::RectanglePlacement imagePlacement = juce::RectanglePlacement::centred;

        JUCE_DECLARE_WEAK_REFERENCEABLE (SVGComponent)
    };

}
