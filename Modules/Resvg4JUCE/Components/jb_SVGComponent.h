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

    /** Process-wide thread pool shared by every SVG component (single- and
     *  multi-frame) for asynchronous high-resolution rasterisation. Sized to
     *  half the logical CPU count (min 2) so SVG rasterisation parallelises
     *  but doesn't oversubscribe alongside other audio/UI work.
     *
     *  Implemented as a Meyers singleton with the same lifetime as the host
     *  process so that closing and re-opening an editor never waits on
     *  `juce::ThreadPool::removeAllJobs`. Stale jobs from a closed editor
     *  finish their render, post a callAsync that finds a null WeakReference,
     *  and discard their result. */
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
     *  background jobs.
     *
     *  Holding the tree, the cancellation generation counter, and a render
     *  mutex inside a shared_ptr eliminates two classes of bug:
     *
     *   1. The owning component can be destroyed while a background job is
     *      mid-render. The shared_ptr keeps the tree alive until the worker
     *      finishes; the worker reads `generation` directly without ever
     *      dereferencing the (possibly destructed) component.
     *
     *   2. The component's resized() may render a low-resolution placeholder
     *      synchronously while a previous job is still rendering high-res on
     *      a worker thread. The mutex serialises every call into
     *      `Resvg::RenderTree::render`, regardless of which version of the
     *      underlying resvg-c library is linked in. */
    struct AsyncSvgRenderState
    {
        Resvg::RenderTree tree;
        std::mutex renderMutex;
        std::atomic<int> generation { 0 };

        AsyncSvgRenderState() = default;
        AsyncSvgRenderState (Resvg::RenderTree&& movedTree) : tree (std::move (movedTree)) {}
    };

    /**
 * A component that owns an Resvg::RenderTree. On each resize, it renders an Image according to the Components size
 * and displays it according to the placement set through setImagePlacement (default is centred)
 *
 * Rendering happens in two phases. First, a low-resolution image (at the parent's
 * physical scale, no supersampling) is rasterised synchronously so the component
 * has something to draw immediately — the cost matches the pre-supersample
 * baseline. Then a high-resolution supersampled image is rendered on a shared
 * background thread pool and swapped in for sharper output once it is ready.
 *
 * If subsequent resizes invalidate an in-flight job, the stale result is dropped
 * via a generation counter; if a hi-res image for the requested pixel dimensions
 * is already in juce::ImageCache, it is used directly without dispatching a job.
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

        /** Supersample factor over physical pixels. With the parent component
         *  applying a scale transform (e.g. mainComponent.setTransform(scale(uiSize))),
         *  we want the SVG bitmap to have enough source detail that the inevitable
         *  downsample to physical pixels stays crisp. 2x is a sweet spot — costs
         *  4x bitmap memory per icon (icons are small) and gives Lanczos / bilinear
         *  enough headroom to avoid the muddy look of sampling near-identity ratios. */
        static constexpr float supersample = 2.0f;

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

            // Already at the target high-res for this size? Nothing to do.
            if (cachedImage.isValid()
                && juce::approximatelyEqual (cachedRenderScale, highResScale)
                && cachedImage.getWidth()  == hiW
                && cachedImage.getHeight() == hiH)
                return;

            // Any previously scheduled high-res job is now stale.
            const int myGen = state->generation.fetch_add (1, std::memory_order_relaxed) + 1;

            // Fast path: high-res already in the shared cache (e.g. another instance rendered it).
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

            // Try to produce a low-res placeholder synchronously so the component
            // paints something immediately. We never block the message thread on
            // an in-flight worker — if the cache misses and the render mutex is
            // contended we skip the placeholder and rely on the async high-res
            // path below to land first; the component paints whatever it had
            // (or nothing) until then.
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

            // Schedule the high-res render in the background.
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

        /** Called on the message thread when an async high-res render lands. */
        void publishHighRes (juce::Image image, float scale, juce::Rectangle<float> bounds)
        {
            cachedImage       = std::move (image);
            cachedRenderScale = scale;
            cachedImageBounds = bounds;
            repaint();
        }

        /** Background job that rasterises the SVG at the supersampled resolution.
         *
         *  The job holds a shared_ptr to the AsyncSvgRenderState so that the tree
         *  and its render mutex outlive the owning component if necessary. It
         *  never reads any field of *this from the worker thread — the
         *  cancellation check goes through `state->generation`. The juce::Image
         *  is published back on the message thread via callAsync, which is the
         *  only place the WeakReference<SVGComponent> is dereferenced. */
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

                // Cancellation check: have we been superseded by a newer schedule
                // (or by component destruction, which also bumps the counter)?
                if (state->generation.load (std::memory_order_relaxed) != gen)
                    return jobHasFinished;

                juce::Image rendered;
                if (cacheKey != 0)
                    rendered = juce::ImageCache::getFromHashCode (cacheKey);

                if (! rendered.isValid())
                {
                    std::lock_guard<std::mutex> lock (state->renderMutex);

                    // Re-check after acquiring the lock — the synchronous render
                    // path on the message thread may have produced a result while
                    // we were queued, or the component may have gone away.
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
                    // The publish step runs on the message thread, which is also
                    // the thread that destructs the SVGComponent. weak.get() and
                    // any subsequent member access are therefore race-free.
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

        int64_t contentHash = 0;

        juce::Image cachedImage;
        juce::Rectangle<float> cachedImageBounds;
        float cachedRenderScale = 0.0f;

        juce::RectanglePlacement imagePlacement = juce::RectanglePlacement::centred;

        JUCE_DECLARE_WEAK_REFERENCEABLE (SVGComponent)
    };

}
