/*
 * Buffer.h
 *
 *  Created on: 18 Apr 2014
 *      Author: lester
 */

#ifndef BUFFER_H_
#define BUFFER_H_

#include "stdint.h"

#if 0

//using namespace uexception;

/* utf8
 * read char will use a iterator that can be pop or next type
 * utf8 will leave the it on the last read byte ready to jump to next utf8-char
 * parent will do ++ and ask to utf8 for byte.
 *
 * two ways utf8:read and advance pointer utf8, decode and jump waste bytes.  pointer is stop on last usefull byte
 * or utf8 advance pointer start reading no good
 *
 * on eof a new exception is throw as invalid ut8
 *
 *
 */

namespace io
{
/*
 * Block is a block of data can be on heap or stack, it is a template object

 * Buffer is a object holding pointer and size
 * support read and write iterators
 * must be constructed from a Block
 * it does not release memory block
 *
 * InBuffer is derived from buffer, define iterator in read direction only
 * is attached to an io device to read data when it is necessary
 *
 */

/*
 * Specialization for size equal 0
 * A data buffer can be static from stack or dynamic from heap
 *
 * _basic_buffer is a base class template with specialization
 * can hold a pointer plus a size or
 * a array
 */

class Buffer;

/*
 * Stack block of memory
 */
template<int N = 0>
class Block
{
	friend class Buffer;
protected:
	char data[N];

	Block(const Block &b) = delete;
	Block(Block &&b) = delete;						// move constructor
	Block& operator=(const Block& b) = delete;		// assignment
	Block& operator=(Block&& b) = delete;			// move assignment
public:
	Block()
	{

	}
	uint32_t size() const
	{
		return N;
	}
};

/*
 * Specialization for heap or dynamic allocate memory
 */
template<>
class Block<0>
{
	friend class Buffer;
protected:
	char * const data;
	uint32_t const _size;

	uint32_t size() const
	{
		return _size;
	}

	Block(const Block &b) = delete;
	Block(Block &&b) = delete;						// move constructor
	Block& operator=(const Block& b) = delete;		// assignment
	Block& operator=(Block&& b) = delete;			// move assignment
public:
	Block(void* const data, uint32_t size) :
			data(reinterpret_cast<char*>(data)), _size(size)
	{

	}
};

/**
 * Block of memory or buffer
 */
class cBlock
{
	const void * const _data;	///< pointer to data
	const size_t _size;		///< size of data pointed to
public:
	/**
	 * Constructor
	 * @param [in] data - pointer to current data
	 * @param [in] size - len of this buffer
	 */
	cBlock(const void* data, size_t size) :
			_data(data), _size(size)
	{

	}
	/**
	 * Get pointer to data
	 * @return a pointer to current data
	 */
	const void * data() const
	{
		return _data;
	}
	/**
	 * Get size or len of data pointed
	 * @return size in bytes of the block
	 */
	size_t size() const
	{
		return _size;
	}

};

/*
 * A general input/output buffer is implement
 *
 * Use this to hold a memory area use to read from io port
 * max size and current position are use to add data to the end
 *
 * use as write buffer, maxsize is the current size of the buffer and size will be use as written count
 *   |-----------|---------------------------|-------------------------|
 *   0          rd                           wr                        max
 *      old data     written, can be read        space for write
 */

class Buffer
{
protected:
	char * const data;		// pointer to data buffer
	const uint32_t size;	// size of data buffer
	bool throw_overflow = true;

	uint32_t wr_idx = 0;	// read pointer
	uint32_t rd_idx = 0;	// write pointer
public:
	template<int N>
	explicit Buffer(Block<N> &b) :
			data(b.data), size(b.size()) //= delete		// copy constructor only for temporal
	{

	}
	explicit Buffer(char* data, const uint32_t size) :
			data(data), size(size)
	{

	}

	~Buffer()
	{

	}
	void dont_throw()
	{
		throw_overflow = false;
	}
	// get a copy o this buffer to avoid modifications
	const Buffer& copy()
	{
		return *this;
	}
	Buffer(const Buffer &b) = delete;					// move constructor
	Buffer(Buffer &&b) = delete;					// move constructor
	Buffer& operator=(const Buffer& b) = delete;	// assignment
	Buffer& operator=(Buffer&& b) = delete;			// move assignment

	class iterator
	{
		friend class Buffer;
		char * pos;
		iterator(char* pos) :
				pos(pos)
		{
		}
	public:
		iterator(const iterator& i) :
				pos(i.pos)
		{
		}
		iterator & operator ++()
		{
			++pos;
			return *this;
		}
		iterator operator ++(int)
		{
			return
			{	pos++};
		}
		/*
		 * Derefence iterator
		 */
		char &operator *()
		{
			return *pos;
		}
		/*
		 *  convert iterator to char *
		 */
		operator const char*() const
		{
			return pos;
		}
		/*
		 * comparation functions iterators
		 */
		bool operator ==(const iterator& it) const
		{
			return (pos == it.pos);
		}
		bool operator !=(const iterator& it) const
		{
			return (pos != it.pos);
		}
		bool operator >(const iterator& it) const
		{
			return (pos > it.pos);
		}
		/*
		 * adjust iterator position
		 */
		iterator operator+(int i)
		{
			return
			{	pos + i};
		}
		int operator-(const iterator& it)
		{
			return pos - it.pos;
		}
		iterator& operator-=(int i)
		{
			pos -= i;
			return *this;
		}

	};
	// iterators
	iterator begin()
	{
		return
		{	data};
	}
	iterator end()
	{
		return
		{	data + size};
	}
	// read begin iterator
	iterator read_begin()
	{
		return
		{	data+rd_idx};
	}
	// read end iterator
	iterator read_end()
	{
		return
		{	data+wr_idx};
	}
	// write iterators
	iterator write_begin()
	{
		return
		{	data+wr_idx};
	}
	iterator write_end()
	{
		return
		{	data+size};
	}
	// read operations implies move data to beginning all iterator must be adjusted
	void adjust(iterator& it)
	{
		it.pos -= rd_idx;
	}

	void getWriteBuffer(char* &buffer, uint32_t &len)
	{
		if (rd_idx >= wr_idx)
			rd_idx = wr_idx = 0;
		buffer = data + wr_idx;
		len = size - wr_idx;
	}
	void getReadBuffer(char* &buffer, uint32_t &len)
	{
		buffer = data + rd_idx;
		len = wr_idx - rd_idx;
	}

	// get rd wr buffer
	void* getW() const
	{
		return data + wr_idx;
	}
	uint32_t getWSize() const
	{
		return size - wr_idx;
	}
	void* getR() const
	{
		return data + rd_idx;
	}
	uint32_t getRSize() const
	{
		return wr_idx - rd_idx;
	}
	// indicating how many data has been written
	void updateW(int count)
	{
		wr_idx += count;
		if (wr_idx > size)
			wr_idx = size;
	}
	// update read, how many data has been read
	void updateR(int count)
	{
		rd_idx += count;
		if (rd_idx >= wr_idx)
			rd_idx = wr_idx;
	}
	// update read pointer using absolute position
	void updateR(const iterator& it)
	{
		rd_idx = it.pos - data;
	}

	// move memory down
	void compact()
	{
		if (rd_idx)
		{
			// check if buffer is empty
			if (rd_idx == wr_idx)
				rd_idx = wr_idx = 0;
			else
			{
				memmove(data, data + rd_idx, wr_idx - rd_idx);
				wr_idx -= rd_idx;
				rd_idx = 0;
			}
		}
	}
	/*!
	 * clear rd and wr pointers
	 * as discard data because is not valid
	 */
	void reset()
	{
		rd_idx = wr_idx = 0;
	}

	Buffer& operator<<(const fstring& s);
	Buffer& operator<<(const char* s);
	Buffer& operator<<(const std::string& s)
	{
		return operator<<(s.c_str());
	}
	Buffer& operator<<(const int i)
	{
		updateW(snprintf(reinterpret_cast<char*>(getW()), getWSize(), "%d", i));
		return *this;
	}
	Buffer& operator<<(const unsigned int i)
	{
		updateW(snprintf(reinterpret_cast<char*>(getW()), getWSize(), "%u", i));
		return *this;
	}
	Buffer& operator<<(const double d)
	{
		updateW(snprintf(reinterpret_cast<char*>(getW()), getWSize(), "%f", d));
		return *this;
	}
	Buffer& operator<<(const char c)
	{
		if (wr_idx < size)
		{
			data[wr_idx] = c;
			++wr_idx;
		}
		return *this;
	}
	Buffer& operator<<(const unsigned long long l)
	{
		updateW(snprintf(reinterpret_cast<char*>(getW()), getWSize(), "%lld", l));
		return *this;
	}

	template<int N>
	Buffer& operator<<(const Block<N> &b);
	Buffer& operator<<(const Buffer& b);
	Buffer& operator<<(const cBlock& b);
	Buffer& operator<<(const _error_info& e)
	{
		updateW(e.print(reinterpret_cast<char*>(getW()), getWSize()));
		return *this;
	}
	Buffer& operator<<(const error_base& e)
	{
		updateW(e.print(reinterpret_cast<char*>(getW()), getWSize()));
		return *this;
	}
	Buffer& operator<<(const MonotonicTime& t)
	{
		updateW(
		snprintf(reinterpret_cast<char*>(getW()), getWSize(), "%u:%02u %02u.%03u", t.getH(), t.getM(), t.getS(), t.getMs()));
		return *this;

	}
	MonotonicTime t;
private:
	// use this constructor just only controller situation
	explicit Buffer(const Buffer* b) :
			data(b->data), size(b->size), throw_overflow(b->throw_overflow), wr_idx(b->wr_idx), rd_idx(b->rd_idx)
	{

	}
};

}
/* namespace IO */
#endif

#endif /* BUFFER_H_ */
