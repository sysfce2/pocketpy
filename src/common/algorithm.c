#include "pocketpy/common/algorithm.h"
#include <string.h>
#include <stdlib.h>

static void merge(void* a_begin,
                  void* a_end,
                  void* b_begin,
                  void* b_end,
                  void* res,
                  int elem_size,
                  int (*cmp)(const void* a, const void* b)) {
    void *a = a_begin, *b = b_begin, *r = res;
    while(a < a_end && b < b_end) {
        if(cmp(a, b) <= 0) {
            memcpy(r, a, elem_size);
            a += elem_size;
        } else {
            memcpy(r, b, elem_size);
            b += elem_size;
        }
        r += elem_size;
    }

    // one of the arrays is empty
    for(; a < a_end; a += elem_size, r += elem_size)
        memcpy(r, a, elem_size);
    for(; b < b_end; b += elem_size, r += elem_size)
        memcpy(r, b, elem_size);
}

void c11__stable_sort(void* ptr,
                      int count,
                      int elem_size,
                      int (*cmp)(const void* a, const void* b)) {
    // merge sort
    void* tmp = malloc(count * elem_size);
    for(int seg = 1; seg < count; seg *= 2) {
        for(void* a = ptr; a < ptr + (count - seg) * elem_size; a += 2 * seg * elem_size) {
            void* b = a + seg * elem_size, *a_end = b, *b_end = b + seg * elem_size;
			if (b_end > ptr + count * elem_size)
				b_end = ptr + count * elem_size;
			merge(a, a_end, b, b_end, tmp, elem_size, cmp);
			memcpy(a, tmp, b_end - a);
        }
    }
	free(tmp);
}
