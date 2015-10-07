#include "stdafx.h"
#ifdef DX12_SUPPORT
#include "BufferUtils.h"


#define MIN2(x, y) ((x) < (y)) ? (x) : (y)
#define MAX2(x, y) ((x) > (y)) ? (x) : (y)


inline
bool overlaps(const std::pair<size_t, size_t> &range1, const std::pair<size_t, size_t> &range2)
{
	return !(range1.second < range2.first || range2.second < range1.first);
}

std::vector<VertexBufferFormat> FormatVertexData(const rsx::data_array_format_info *array_format_info, size_t *vertex_data_size)
{
	std::vector<VertexBufferFormat> Result;
	for (int index = 0; index < rsx::limits::vertex_count; ++index)
	{
		const rsx::data_array_format_info &vertexData = array_format_info[index];
		if (!vertexData.array) continue; // disabled or not a vertex array

		u32 offset = rsx::method_registers[NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + index];
		u32 address = rsx::get_address(offset & 0x7fffffff, offset >> 31);
		if (!address) continue;

		u32 base_offset = rsx::method_registers[NV4097_SET_VERTEX_DATA_BASE_OFFSET];

		size_t elementCount = vertex_data_size[index];

		// If there is a single element, stride is 0, use the size of element instead
		size_t stride = vertexData.stride;
		size_t elementSize = rsx::get_vertex_type_size(vertexData.type);
		size_t start = offset + base_offset;
		size_t end = start + elementSize * vertexData.size + (elementCount - 1) * stride - 1;
		std::pair<size_t, size_t> range = std::make_pair(start, end);
		assert(start < end);
		bool isMerged = false;

		for (VertexBufferFormat &vbf : Result)
		{
			if (overlaps(vbf.range, range) && vbf.stride == stride)
			{
				// Extend buffer if necessary
				vbf.range.first = MIN2(vbf.range.first, range.first);
				vbf.range.second = MAX2(vbf.range.second, range.second);
				vbf.elementCount = MAX2(vbf.elementCount, elementCount);

				vbf.attributeId.push_back(index);
				isMerged = true;
				break;
			}
		}
		if (isMerged)
			continue;
		VertexBufferFormat newRange = { range, std::vector<size_t>{ (size_t)index }, elementCount, stride };
		Result.emplace_back(newRange);
	}
	return Result;
}

void uploadVertexData(const VertexBufferFormat &vbf, const rsx::data_array_format_info *array_format_info, const std::vector<u8> *array_data, size_t baseOffset, void* bufferMap)
{
	for (int vertex = 0; vertex < vbf.elementCount; vertex++)
	{
		for (size_t attributeId : vbf.attributeId)
		{
			u32 off = rsx::method_registers[NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + attributeId];
			u32 address = rsx::get_address(off & 0x7fffffff, off >> 31);
			if (!address) continue;

			if (!address)
			{
				memcpy(bufferMap, array_data[attributeId].data(), array_data[attributeId].size());
				continue;
			}
			size_t offset = (size_t)address + baseOffset - vbf.range.first;
			size_t tsize = rsx::get_vertex_type_size(array_format_info[attributeId].type);
			size_t size = array_format_info[attributeId].size;
			auto src = vm::get_ptr<const u8>(address + (u32)baseOffset + (u32)vbf.stride * vertex);
			char* dst = (char*)bufferMap + offset + vbf.stride * vertex;

			switch (tsize)
			{
			case 1:
			{
				memcpy(dst, src, size);
				break;
			}

			case 2:
			{
				const u16* c_src = (const u16*)src;
				u16* c_dst = (u16*)dst;
				for (u32 j = 0; j < size; ++j) *c_dst++ = _byteswap_ushort(*c_src++);
				break;
			}

			case 4:
			{
				const u32* c_src = (const u32*)src;
				u32* c_dst = (u32*)dst;
				for (u32 j = 0; j < size; ++j) *c_dst++ = _byteswap_ulong(*c_src++);
				break;
			}
			}
		}
	}
}

template<typename IndexType, typename DstType, typename SrcType>
void expandIndexedTriangleFan(DstType *dst, const SrcType *src, size_t indexCount)
{
	IndexType *typedDst = reinterpret_cast<IndexType *>(dst);
	const IndexType *typedSrc = reinterpret_cast<const IndexType *>(src);
	for (unsigned i = 0; i < indexCount - 2; i++)
	{
		typedDst[3 * i] = typedSrc[0];
		typedDst[3 * i + 1] = typedSrc[i + 2 - 1];
		typedDst[3 * i + 2] = typedSrc[i + 2];
	}
}

template<typename IndexType, typename DstType, typename SrcType>
void expandIndexedQuads(DstType *dst, const SrcType *src, size_t indexCount)
{
	IndexType *typedDst = reinterpret_cast<IndexType *>(dst);
	const IndexType *typedSrc = reinterpret_cast<const IndexType *>(src);
	for (unsigned i = 0; i < indexCount / 4; i++)
	{
		// First triangle
		typedDst[6 * i] = typedSrc[4 * i];
		typedDst[6 * i + 1] = typedSrc[4 * i + 1];
		typedDst[6 * i + 2] = typedSrc[4 * i + 2];
		// Second triangle
		typedDst[6 * i + 3] = typedSrc[4 * i + 2];
		typedDst[6 * i + 4] = typedSrc[4 * i + 3];
		typedDst[6 * i + 5] = typedSrc[4 * i];
	}
}

// Only handle quads and triangle fan now
bool isNativePrimitiveMode(unsigned m_draw_mode)
{
	switch (m_draw_mode)
	{
	default:
	case CELL_GCM_PRIMITIVE_POINTS:
	case CELL_GCM_PRIMITIVE_LINES:
	case CELL_GCM_PRIMITIVE_LINE_LOOP:
	case CELL_GCM_PRIMITIVE_LINE_STRIP:
	case CELL_GCM_PRIMITIVE_TRIANGLES:
	case CELL_GCM_PRIMITIVE_TRIANGLE_STRIP:
	case CELL_GCM_PRIMITIVE_QUAD_STRIP:
	case CELL_GCM_PRIMITIVE_POLYGON:
		return true;
	case CELL_GCM_PRIMITIVE_TRIANGLE_FAN:
	case CELL_GCM_PRIMITIVE_QUADS:
		return false;
	}
}

size_t getIndexCount(unsigned m_draw_mode, unsigned initial_index_count)
{
	// Index count
	if (isNativePrimitiveMode(m_draw_mode))
		return initial_index_count;

	switch (m_draw_mode)
	{
	case CELL_GCM_PRIMITIVE_TRIANGLE_FAN:
		return (initial_index_count - 2) * 3;
	case CELL_GCM_PRIMITIVE_QUADS:
		return (6 * initial_index_count) / 4;
	default:
		return 0;
	}
}


void uploadIndexData(unsigned m_draw_mode, unsigned index_type, void* indexBuffer, void* bufferMap, unsigned element_count)
{
	if (indexBuffer != nullptr)
	{
		switch (m_draw_mode)
		{
		case CELL_GCM_PRIMITIVE_POINTS:
		case CELL_GCM_PRIMITIVE_LINES:
		case CELL_GCM_PRIMITIVE_LINE_LOOP:
		case CELL_GCM_PRIMITIVE_LINE_STRIP:
		case CELL_GCM_PRIMITIVE_TRIANGLES:
		case CELL_GCM_PRIMITIVE_TRIANGLE_STRIP:
		case CELL_GCM_PRIMITIVE_QUAD_STRIP:
		case CELL_GCM_PRIMITIVE_POLYGON:
		{
			size_t indexSize = (index_type == CELL_GCM_DRAW_INDEX_ARRAY_TYPE_32) ? 4 : 2;
			memcpy(bufferMap, indexBuffer, indexSize * element_count);
			return;
		}
		case CELL_GCM_PRIMITIVE_TRIANGLE_FAN:
			switch (index_type)
			{
			case CELL_GCM_DRAW_INDEX_ARRAY_TYPE_32:
				expandIndexedTriangleFan<unsigned int>(bufferMap, indexBuffer, element_count);
				return;
			case CELL_GCM_DRAW_INDEX_ARRAY_TYPE_16:
				expandIndexedTriangleFan<unsigned short>(bufferMap, indexBuffer, element_count);
				return;
			default:
				abort();
				return;
			}
		case CELL_GCM_PRIMITIVE_QUADS:
			switch (index_type)
			{
			case CELL_GCM_DRAW_INDEX_ARRAY_TYPE_32:
				expandIndexedQuads<unsigned int>(bufferMap, indexBuffer, element_count);
				return;
			case CELL_GCM_DRAW_INDEX_ARRAY_TYPE_16:
				expandIndexedQuads<unsigned short>(bufferMap, indexBuffer, element_count);
				return;
			default:
				abort();
				return;
			}
		}
	}
	else
	{
		unsigned short *typedDst = static_cast<unsigned short *>(bufferMap);
		switch (m_draw_mode)
		{
		case CELL_GCM_PRIMITIVE_TRIANGLE_FAN:
			for (unsigned i = 0; i < (element_count - 2); i++)
			{
				typedDst[3 * i] = 0;
				typedDst[3 * i + 1] = i + 2 - 1;
				typedDst[3 * i + 2] = i + 2;
			}
			return;
		case CELL_GCM_PRIMITIVE_QUADS:
			for (unsigned i = 0; i < element_count / 4; i++)
			{
				// First triangle
				typedDst[6 * i] = 4 * i;
				typedDst[6 * i + 1] = 4 * i + 1;
				typedDst[6 * i + 2] = 4 * i + 2;
				// Second triangle
				typedDst[6 * i + 3] = 4 * i + 2;
				typedDst[6 * i + 4] = 4 * i + 3;
				typedDst[6 * i + 5] = 4 * i;
			}
			return;
		}
	}
}
#endif