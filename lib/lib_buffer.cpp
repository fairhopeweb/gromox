// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <gromox/defs.h>
#include <gromox/util.hpp>
#include <gromox/lib_buffer.hpp>

static constexpr auto wsize_al = roundup(WSIZE, sizeof(std::max_align_t));

/*
 *	init a buffer pool with specified item size and number
 *
 *	@param	
 *		item_size	the size of the elemenet buffer size
 *		item_num	the number of element buffer
 *
 *	@return			
 *		pointer to LIB_BUFFER	structure
 *		NULL if error happened
 */
LIB_BUFFER* lib_buffer_init(size_t item_size, size_t item_num, BOOL is_thread_safe)
{
	void*	head_listp		= NULL;
	
	if (item_size <= 0 || item_num <= 0) {
		debug_info("[lib_buffer]: lib_buffer_init, invalid parameter");
		return NULL;
	}
	auto lib_buffer = static_cast<LIB_BUFFER *>(malloc(sizeof(LIB_BUFFER)));
	if (lib_buffer == nullptr) {
		debug_info("[lib_buffer]: lib_buffer_init, malloc lib_buffer fail");
		return NULL;
	}

	auto item_size_al = roundup(item_size, sizeof(std::max_align_t));
	head_listp = malloc((item_size_al + wsize_al) * item_num);
	if (head_listp == nullptr) {
		debug_info("[lib_buffer]: lib_buffer_init, malloc head_listp fail");
		free(lib_buffer);
		return NULL;
	}

	pthread_mutex_init(&lib_buffer->m_mutex, NULL);
	memset(head_listp, 0, (item_size_al + wsize_al) * item_num);
	lib_buffer->heap_list_head	= head_listp;
	lib_buffer->cur_heap_head	= head_listp;

	lib_buffer->free_list_head	= NULL;
	lib_buffer->free_list_size	= 0;
	lib_buffer->allocated_num	= 0;
	lib_buffer->item_size		= item_size;
	lib_buffer->item_num		= item_num;
	lib_buffer->is_thread_safe	= is_thread_safe;
	return lib_buffer;
}

/*
 *	free a buffer pool
 *	
 *	@param	
 *		m_buf [in]	the buffer pool to release
 *
 */
void lib_buffer_free(LIB_BUFFER* m_buf)
{
#ifdef _DEBUG_UMTA
	if (NULL == m_buf) {
		debug_info("[lib_buffer]: lib_buffer_free, param NULL");
		return;
	}
#endif
	pthread_mutex_destroy(&m_buf->m_mutex);
	free(m_buf->heap_list_head);
	free(m_buf);
}

/*
 *	allocate a buffer from the specified buffer pool the buffer size
 *	is determined when lib_buffer_init
 *
 *	@param	
 *		m_buf [in]	the buffer pool where to allocate the buffer
 *
 *	@return		
 *		the pointer to the new allocated buffer NULL if we allocate
 *		more buffers than specified in lib_buffer_init.
 */
void *lib_buffer_get1(LIB_BUFFER *m_buf)
{
	void	*ret_buf	= NULL;
	char	*phead		= NULL;

#ifdef _DEBUG_UMTA
	if (NULL == m_buf) {
		debug_info("[lib_buffer]: lib_buffer_get, param NULL");
		return NULL;
	}
#endif
	if (TRUE == m_buf->is_thread_safe) {
		pthread_mutex_lock(&m_buf->m_mutex);
	}

	auto item_size_al = roundup(m_buf->item_size, sizeof(std::max_align_t));
	if (m_buf->free_list_size > 0) {
		phead	= (char *)m_buf->free_list_head;
		ret_buf = m_buf->free_list_head;
		memcpy(&phead, phead + item_size_al, sizeof(void *));
#ifdef _DEBUG_UMTA
		/* check memory */
		memset(ret_buf + item_size_al, 0, sizeof(void *));
#endif

		m_buf->free_list_head  = (void*)phead;
		m_buf->free_list_size -= 1;
		m_buf->allocated_num  += 1;
		if (TRUE == m_buf->is_thread_safe) {
			pthread_mutex_unlock(&m_buf->m_mutex);
		}
		return ret_buf;
		
	} 

	
	if (m_buf->allocated_num >= m_buf->item_num) {
		debug_info("[lib_buffer]: the total allocated buffer num"
			" is larger than the initializing");
		if (TRUE == m_buf->is_thread_safe) {
			pthread_mutex_unlock(&m_buf->m_mutex);
		}
		return NULL;
	}

	phead	= (char*)m_buf->cur_heap_head;
	ret_buf = m_buf->cur_heap_head;
	memset(phead + item_size_al, 0, sizeof(void *));
	phead  += item_size_al + wsize_al;
	m_buf->cur_heap_head	= (void*)phead;
	m_buf->allocated_num	+= 1;

	if (TRUE == m_buf->is_thread_safe) {
		pthread_mutex_unlock(&m_buf->m_mutex);
	}
	return ret_buf;
}
/*
 *	return the buffer to the buffer pool
 *
 *	@param	
 *		m_buf [in]	the buffer pool
 *		item  [in]	the buffer to return
 *
 */
void lib_buffer_put1(LIB_BUFFER *m_buf, void *item)
{
	char *pcur_item = NULL;
#ifdef _DEBUG_UMTA
	void *pzero;
#endif

	if (NULL == m_buf || NULL == item) {
		debug_info("[lib_buffer]: lib_buffer_put, param NULL");
		return;
	}
	pcur_item	= (char *)item;
	auto item_size_al = roundup(m_buf->item_size, sizeof(std::max_align_t));
	memset(pcur_item, 0, item_size_al);
#ifdef _DEBUG_UMTA
	/* memory check */
	memcpy(&pzero, pcur_item + item_size_al, sizeof(void *));
	if (pzero != 0) {
		debug_info("[lib_buffer]: lib_buffer memory dump");
	}
#endif

	if (TRUE == m_buf->is_thread_safe) {
		pthread_mutex_lock(&m_buf->m_mutex);
	}
	memcpy(pcur_item + item_size_al, &m_buf->free_list_head, sizeof(void *));
	m_buf->free_list_head = item;
	m_buf->free_list_size += 1;
	m_buf->allocated_num  -= 1;
	if (TRUE == m_buf->is_thread_safe) {
		pthread_mutex_unlock(&m_buf->m_mutex);
	}
}

size_t lib_buffer_get_param(LIB_BUFFER* m_buf, PARAM_TYPE type) {

	size_t	ret_val = 0xFFFFFFFF;

#ifdef _DEBUG_UMTA
	if (NULL == m_buf) {
		debug_info("[lib_buffer]: lib_buffer_get_param, param NULL");
		return ret_val;
	}
#endif
	if (TRUE == m_buf->is_thread_safe) {
		pthread_mutex_lock(&m_buf->m_mutex);
	}

	switch (type) {

	case FREE_LIST_SIZE:
		ret_val = m_buf->free_list_size;
		break;
	case ALLOCATED_NUM:
		ret_val = m_buf->allocated_num;
		break;
	case MEM_ITEM_SIZE:
		ret_val = m_buf->item_size;
		break;
	case MEM_ITEM_NUM:
		ret_val = m_buf->item_num;
		break;
	default:
		debug_info("[lib_buffer]: unknown type %d", type);
	}
	if (TRUE == m_buf->is_thread_safe) {
		pthread_mutex_unlock(&m_buf->m_mutex);
	}
	return ret_val;
}

