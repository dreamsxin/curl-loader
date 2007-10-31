#include <util/vbuffer.h>

#if 0
V_EXPORT int VBUFFER_push_back( VBUFFER *tbl, void *data, size_t data_size)
{
	if ((tbl->end + data_size) < tbl->size) {
		memcpy(tbl->entry + tbl->end, data, data_size);
		tbl->end += data_size;
	} else {
		if ( ((tbl->end + data_size) - tbl->size) < tbl->start) {
			VBUFFER_compact( tbl );
		} else if (tbl->flags) {
			VBUFFER_compact( tbl );
			if (VBUFFER_grow( tbl, tbl->end + data_size - tbl->size)) {
				return -1;
			}
		} else {
			return -1;
		}
		memcpy(tbl->entry + tbl->end, data, data_size);
		tbl->end += data_size;
	}
	return 0;
}



V_INLINE size_t VBUFFER_available_end( VBUFFER *tbl )
{
	return tbl->size - tbl->end;
}

V_INLINE size_t VBUFFER_available_start( VBUFFER *tbl )
{
	return tbl->start;
}

V_INLINE void *VBUFFER_begin( VBUFFER *tbl )
{
	return tbl->entry + tbl->start; 
}

V_INLINE int VBUFFER_move_front( VBUFFER *tbl, size_t distance)
{
	size_t pos = tbl->start + distance;
	if (pos > tbl->end) {
		tbl->start = tbl->end = 0;
		return 0;
	}

	if (pos >= tbl->size) {
		pos = tbl->size -1;
		return -1;
	}

	tbl->start = pos;

	return 0;
}

V_INLINE int VBUFFER_move_back( VBUFFER *tbl, int distance)
{
	size_t pos = tbl->end + distance;
	if (pos < tbl->start) {
		tbl->start = tbl->end = 0;
		return 0;
	}

	if (pos >= tbl->size) {
		pos = tbl->size -1;
		return -1;
	}

	tbl->end = pos;

	return 0;
}

V_INLINE void *VBUFFER_end( VBUFFER *tbl)
{
	return tbl->entry + tbl->end;
}

V_INLINE void VBUFFER_pop_front( VBUFFER *tbl, size_t sz) 
{
	size_t tsize = tbl->end - tbl->start; 
 	sz = tsize > sz ? sz : tsize;
	tbl->start += sz;	
}

V_EXPORT int VBUFFER_push_back(  VBUFFER *tbl, void *data, size_t data_size);

V_INLINE int VBUFFER_grow(  VBUFFER *tbl, size_t grow) 
{
	size_t newsize = tbl->size + grow;
	unsigned char *buf;

	if (!(tbl->flags & VBUFFER_RESIZE)) {
		return -1;
	}

	buf = tbl->ctx->realloc( tbl->entry, newsize );
	
	if (!buf) {
		return -1;
	}

	tbl->entry = buf;
	tbl->size = newsize;
	return 0;

#endif
