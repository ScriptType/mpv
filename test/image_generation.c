#include <stdatomic.h>
#include <libavutil/buffer.h>

#include "test_utils.h"
#include "video/mp_image.h"

int main(void)
{
    AVBufferRef *generation = av_buffer_allocz(sizeof(_Atomic uint64_t));
    assert_true(generation != NULL);
    atomic_init((_Atomic uint64_t *)generation->data, 7);
    struct mp_image *original = mp_image_alloc(IMGFMT_RGBAF16, 4, 4);
    assert_true(original != NULL);
    original->async_generation = av_buffer_ref(generation);
    original->async_frame_generation = 7;
    original->source_pts = INT64_C(9007199254740993);
    original->source_duration = 1001;
    original->source_timebase_num = 1;
    original->source_timebase_den = 30000;
    struct mp_image *retained = mp_image_new_ref(original);
    struct mp_image *copied = mp_image_new_copy(original);
    assert_true(mp_image_is_current(retained));
    assert_true(mp_image_is_current(copied));
    assert_true(copied->source_pts == original->source_pts);
    assert_true(copied->source_duration == 1001);
    assert_true(copied->source_timebase_den == 30000);
    assert_true(mp_image_same_async_identity(original, copied));
    copied->source_pts--;
    assert_false(mp_image_set_async_pair(copied, original));
    copied->source_pts++;
    original->planes[0][0] = 17;
    copied->planes[0][0] = 93;
    assert_true(mp_image_set_async_pair(copied, original));
    struct mp_image *source_variant = mp_image_async_variant(copied, true);
    assert_true(source_variant->planes[0][0] == 17);
    assert_true(source_variant->async_original);
    struct mp_image *enhanced_variant = mp_image_async_variant(source_variant, false);
    assert_true(enhanced_variant->planes[0][0] == 93);
    assert_false(enhanced_variant->async_original);
    assert_true(mp_image_same_async_identity(source_variant, enhanced_variant));
    talloc_free(original);
    // Source destruction cannot invalidate a retained descriptor. A seek updates
    // one shared atomic state, cancelling queued copies without freeing pixels.
    assert_true(mp_image_is_current(retained));
    atomic_store((_Atomic uint64_t *)generation->data, 8);
    assert_false(mp_image_is_current(retained));
    assert_false(mp_image_is_current(copied));
    assert_true(mp_image_async_variant(source_variant, false) == NULL);
    assert_false(mp_image_same_async_identity(source_variant, enhanced_variant));
    av_buffer_unref(&generation);
    assert_false(mp_image_is_current(retained));
    talloc_free(retained);
    talloc_free(copied);
    talloc_free(source_variant);
    talloc_free(enhanced_variant);

    // Equivalent rational timestamps compare exactly across distinct scales.
    generation = av_buffer_allocz(sizeof(_Atomic uint64_t));
    atomic_init((_Atomic uint64_t *)generation->data, 1);
    struct mp_image a = {.async_generation = generation, .async_frame_generation = 1,
        .source_pts = 1001, .source_timebase_num = 1, .source_timebase_den = 30000};
    struct mp_image b = a;
    b.source_pts = 2002; b.source_timebase_den = 60000;
    assert_true(mp_image_same_async_identity(&a, &b));
    b.source_pts++;
    assert_false(mp_image_same_async_identity(&a, &b));
    av_buffer_unref(&generation);
}
