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
    talloc_free(original);
    // Source destruction cannot invalidate a retained descriptor. A seek updates
    // one shared atomic state, cancelling queued copies without freeing pixels.
    assert_true(mp_image_is_current(retained));
    atomic_store((_Atomic uint64_t *)generation->data, 8);
    assert_false(mp_image_is_current(retained));
    assert_false(mp_image_is_current(copied));
    av_buffer_unref(&generation);
    assert_false(mp_image_is_current(retained));
    talloc_free(retained);
    talloc_free(copied);
}
