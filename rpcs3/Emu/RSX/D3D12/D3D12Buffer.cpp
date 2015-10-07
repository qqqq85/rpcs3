#include "stdafx.h"
#if defined(DX12_SUPPORT)
#include "D3D12Buffer.h"
#include "Utilities/Log.h"

#include "D3D12GSRender.h"
#include "d3dx12.h"
#include "../Common/BufferUtils.h"

const int g_vertexCount = 32;

// Where are these type defined ???
static
DXGI_FORMAT getFormat(u8 type, u8 size)
{
	/*static const u32 gl_types[] =
	{
	GL_SHORT,
	GL_FLOAT,
	GL_HALF_FLOAT,
	GL_UNSIGNED_BYTE,
	GL_SHORT,
	GL_FLOAT, // Needs conversion
	GL_UNSIGNED_BYTE,
	};

	static const bool gl_normalized[] =
	{
	GL_TRUE,
	GL_FALSE,
	GL_FALSE,
	GL_TRUE,
	GL_FALSE,
	GL_TRUE,
	GL_FALSE,
	};*/
	static const DXGI_FORMAT typeX1[] =
	{
		DXGI_FORMAT_R16_SNORM,
		DXGI_FORMAT_R32_FLOAT,
		DXGI_FORMAT_R16_FLOAT,
		DXGI_FORMAT_R8_UNORM,
		DXGI_FORMAT_R16_SINT,
		DXGI_FORMAT_R32_FLOAT,
		DXGI_FORMAT_R8_UINT
	};
	static const DXGI_FORMAT typeX2[] =
	{
		DXGI_FORMAT_R16G16_SNORM,
		DXGI_FORMAT_R32G32_FLOAT,
		DXGI_FORMAT_R16G16_FLOAT,
		DXGI_FORMAT_R8G8_UNORM,
		DXGI_FORMAT_R16G16_SINT,
		DXGI_FORMAT_R32G32_FLOAT,
		DXGI_FORMAT_R8G8_UINT
	};
	static const DXGI_FORMAT typeX3[] =
	{
		DXGI_FORMAT_R16G16B16A16_SNORM,
		DXGI_FORMAT_R32G32B32_FLOAT,
		DXGI_FORMAT_R16G16B16A16_FLOAT,
		DXGI_FORMAT_R8G8B8A8_UNORM,
		DXGI_FORMAT_R16G16B16A16_SINT,
		DXGI_FORMAT_R32G32B32_FLOAT,
		DXGI_FORMAT_R8G8B8A8_UINT
	};
	static const DXGI_FORMAT typeX4[] =
	{
		DXGI_FORMAT_R16G16B16A16_SNORM,
		DXGI_FORMAT_R32G32B32A32_FLOAT,
		DXGI_FORMAT_R16G16B16A16_FLOAT,
		DXGI_FORMAT_R8G8B8A8_UNORM,
		DXGI_FORMAT_R16G16B16A16_SINT,
		DXGI_FORMAT_R32G32B32A32_FLOAT,
		DXGI_FORMAT_R8G8B8A8_UINT
	};

	switch (size)
	{
	case 1:
		return typeX1[type];
	case 2:
		return typeX2[type];
	case 3:
		return typeX3[type];
	case 4:
		return typeX4[type];
	default:
		LOG_ERROR(RSX, "Wrong size for vertex attrib : %d", size);
		return DXGI_FORMAT();
	}
}

static
std::vector<D3D12_INPUT_ELEMENT_DESC> getIALayout(ID3D12Device *device, const std::vector<VertexBufferFormat> &vertexBufferFormat, const rsx::data_array_format_info *m_vertex_data)
{
	std::vector<D3D12_INPUT_ELEMENT_DESC> result;
	u32 base_offset = rsx::method_registers[NV4097_SET_VERTEX_DATA_BASE_OFFSET];
	for (size_t inputSlot = 0; inputSlot < vertexBufferFormat.size(); inputSlot++)
	{
		for (size_t attributeId : vertexBufferFormat[inputSlot].attributeId)
		{
			const rsx::data_array_format_info &vertexData = m_vertex_data[attributeId];
			u32 offset = rsx::method_registers[NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + attributeId];
			u32 address = rsx::get_address(offset & 0x7fffffff, offset >> 31);
			D3D12_INPUT_ELEMENT_DESC IAElement = {};
			IAElement.SemanticName = "TEXCOORD";
			IAElement.SemanticIndex = (UINT)attributeId;
			IAElement.InputSlot = (UINT)inputSlot;
			IAElement.Format = getFormat(vertexData.type - 1, vertexData.size);
			IAElement.AlignedByteOffset = (UINT)(address + base_offset - vertexBufferFormat[inputSlot].range.first);
			IAElement.InputSlotClass = (address > 0) ? D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA : D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
			IAElement.InstanceDataStepRate = (address > 0) ? 0 : 0;
			result.push_back(IAElement);
		}
	}
	return result;
}

// D3D12GS member handling buffers


/**
 * Suballocate a new vertex buffer with attributes from vbf using vertexIndexHeap as storage heap.
 */
static
D3D12_GPU_VIRTUAL_ADDRESS createVertexBuffer(const VertexBufferFormat &vbf, const rsx::data_array_format_info *vertexData, const std::vector<u8> *array_data, size_t baseOffset, ID3D12Device *device, DataHeap<ID3D12Resource, 65536> &vertexIndexHeap)
{
	size_t subBufferSize = vbf.range.second - vbf.range.first + 1;
	// Make multiple of stride
	if (vbf.stride)
		subBufferSize = ((subBufferSize + vbf.stride - 1) / vbf.stride) * vbf.stride;
	assert(vertexIndexHeap.canAlloc(subBufferSize));
	size_t heapOffset = vertexIndexHeap.alloc(subBufferSize);

	void *buffer;
	ThrowIfFailed(vertexIndexHeap.m_heap->Map(0, &CD3DX12_RANGE(heapOffset, heapOffset + subBufferSize), (void**)&buffer));
	void *bufferMap = (char*)buffer + heapOffset;
	uploadVertexData(vbf, vertexData, array_data, baseOffset, bufferMap);
	vertexIndexHeap.m_heap->Unmap(0, &CD3DX12_RANGE(heapOffset, heapOffset + subBufferSize));
	return vertexIndexHeap.m_heap->GetGPUVirtualAddress() + heapOffset;
}

std::vector<D3D12_VERTEX_BUFFER_VIEW> D3D12GSRender::UploadVertexBuffers(bool indexed_draw)
{
	std::vector<D3D12_VERTEX_BUFFER_VIEW> result;
	u32 base_offset = rsx::method_registers[NV4097_SET_VERTEX_DATA_BASE_OFFSET];
	const std::vector<VertexBufferFormat> &vertexBufferFormat = FormatVertexData(vertex_arrays_info, m_vertexBufferSize);
	m_IASet = getIALayout(m_device.Get(), vertexBufferFormat, vertex_arrays_info);

	for (size_t buffer = 0; buffer < vertexBufferFormat.size(); buffer++)
	{
		const VertexBufferFormat &vbf = vertexBufferFormat[buffer];
		// Make multiple of stride
		size_t subBufferSize = vbf.range.second - vbf.range.first + 1;
		if (vbf.stride)
			subBufferSize = ((subBufferSize + vbf.stride - 1) / vbf.stride) * vbf.stride;

		D3D12_GPU_VIRTUAL_ADDRESS virtualAddress = createVertexBuffer(vbf, vertex_arrays_info, vertex_arrays, base_offset, m_device.Get(), m_vertexIndexData);
		m_timers.m_bufferUploadSize += subBufferSize;

		D3D12_VERTEX_BUFFER_VIEW vertexBufferView = {};
		vertexBufferView.BufferLocation = virtualAddress;
		vertexBufferView.SizeInBytes = (UINT)subBufferSize;
		vertexBufferView.StrideInBytes = (UINT)vbf.stride;
		result.push_back(vertexBufferView);
	}

	return result;
}

D3D12_INDEX_BUFFER_VIEW D3D12GSRender::uploadIndexBuffers(bool indexed_draw)
{
	D3D12_INDEX_BUFFER_VIEW indexBufferView = {};

	// No need for index buffer
	if (!indexed_draw && isNativePrimitiveMode(draw_mode))
	{
		m_renderingInfo.m_indexed = false;
		m_renderingInfo.m_count = vertex_draw_count;
		m_renderingInfo.m_baseVertex = 0;
		return indexBufferView;
	}

	m_renderingInfo.m_indexed = true;

	u32 index_type = rsx::method_registers[NV4097_SET_INDEX_ARRAY_DMA] >> 4;

	// Index type
	size_t indexSize;
	if (!indexed_draw)
	{
		indexBufferView.Format = DXGI_FORMAT_R16_UINT;
		indexSize = 2;
	}
	else
	{
		switch (index_type)
		{
		default: abort();
		case CELL_GCM_DRAW_INDEX_ARRAY_TYPE_16:
			indexBufferView.Format = DXGI_FORMAT_R16_UINT;
			indexSize = 2;
			break;
		case CELL_GCM_DRAW_INDEX_ARRAY_TYPE_32:
			indexBufferView.Format = DXGI_FORMAT_R32_UINT;
			indexSize = 4;
			break;
		}
	}

	// Index count
	m_renderingInfo.m_count = getIndexCount(draw_mode, indexed_draw ? (u32)(vertex_index_array.size() / indexSize) : vertex_draw_count);

	// Base vertex
	if (!indexed_draw && isNativePrimitiveMode(draw_mode))
		m_renderingInfo.m_baseVertex = 0;
	else
		m_renderingInfo.m_baseVertex = 0;

	// Alloc
	size_t subBufferSize = align(m_renderingInfo.m_count * indexSize, 64);

	assert(m_vertexIndexData.canAlloc(subBufferSize));
	size_t heapOffset = m_vertexIndexData.alloc(subBufferSize);

	void *buffer;
	ThrowIfFailed(m_vertexIndexData.m_heap->Map(0, &CD3DX12_RANGE(heapOffset, heapOffset + subBufferSize), (void**)&buffer));
	void *bufferMap = (char*)buffer + heapOffset;
	uploadIndexData(draw_mode, index_type, indexed_draw ? vertex_index_array.data() : nullptr, bufferMap, indexed_draw ? (u32)(vertex_index_array.size() / indexSize) : vertex_draw_count);
	m_vertexIndexData.m_heap->Unmap(0, &CD3DX12_RANGE(heapOffset, heapOffset + subBufferSize));
	m_timers.m_bufferUploadSize += subBufferSize;
	indexBufferView.SizeInBytes = (UINT)subBufferSize;
	indexBufferView.BufferLocation = m_vertexIndexData.m_heap->GetGPUVirtualAddress() + heapOffset;
	return indexBufferView;
}

void D3D12GSRender::setScaleOffset()
{
	float scaleOffsetMat[16] =
	{
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, -1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	};

	int viewport_x = int(rsx::method_registers[NV4097_SET_VIEWPORT_HORIZONTAL] & 0xffff);
	int viewport_y = int(rsx::method_registers[NV4097_SET_VIEWPORT_VERTICAL] & 0xffff);
	int viewport_w = int(rsx::method_registers[NV4097_SET_VIEWPORT_HORIZONTAL] >> 16);
	int viewport_h = int(rsx::method_registers[NV4097_SET_VIEWPORT_VERTICAL] >> 16);

	float viewport_offset_x = (f32&)rsx::method_registers[NV4097_SET_VIEWPORT_OFFSET + 0];
	float viewport_offset_y = (f32&)rsx::method_registers[NV4097_SET_VIEWPORT_OFFSET + 1];
	float viewport_offset_z = (f32&)rsx::method_registers[NV4097_SET_VIEWPORT_OFFSET + 2];
	float viewport_offset_w = (f32&)rsx::method_registers[NV4097_SET_VIEWPORT_OFFSET + 3];

	float viewport_scale_x = (f32&)rsx::method_registers[NV4097_SET_VIEWPORT_SCALE + 0];
	float viewport_scale_y = (f32&)rsx::method_registers[NV4097_SET_VIEWPORT_SCALE + 1];
	float viewport_scale_z = (f32&)rsx::method_registers[NV4097_SET_VIEWPORT_SCALE + 2];
	float viewport_scale_w = (f32&)rsx::method_registers[NV4097_SET_VIEWPORT_SCALE + 3];

	float width = f32(rsx::method_registers[NV4097_SET_SURFACE_CLIP_HORIZONTAL] >> 16);
	float height = f32(rsx::method_registers[NV4097_SET_SURFACE_CLIP_VERTICAL] >> 16);

	//Scale
	scaleOffsetMat[0] = viewport_scale_x * 2.f / viewport_w;
	scaleOffsetMat[5] = viewport_scale_y * 2.f / viewport_h;
	scaleOffsetMat[10] = viewport_scale_z;

	// Offset
	scaleOffsetMat[3] = viewport_offset_x * 2.f / viewport_w - 1.f;
	scaleOffsetMat[7] = viewport_offset_y * 2.f / viewport_h - 1.f;
	scaleOffsetMat[11] = viewport_offset_z - .5f;

	assert(m_constantsData.canAlloc(256));
	size_t heapOffset = m_constantsData.alloc(256);

	// Scale offset buffer
	// Separate constant buffer
	void *scaleOffsetMap;
	ThrowIfFailed(m_constantsData.m_heap->Map(0, &CD3DX12_RANGE(heapOffset, heapOffset + 256), &scaleOffsetMap));
	streamToBuffer((char*)scaleOffsetMap + heapOffset, scaleOffsetMat, 16 * sizeof(float));
	int isAlphaTested = rsx::method_registers[NV4097_SET_ALPHA_TEST_ENABLE];
	float alpha_ref = .5f;
	memcpy((char*)scaleOffsetMap + heapOffset + 16 * sizeof(float), &isAlphaTested, sizeof(int));
	memcpy((char*)scaleOffsetMap + heapOffset + 17 * sizeof(float), &alpha_ref, sizeof(float));
	m_constantsData.m_heap->Unmap(0, &CD3DX12_RANGE(heapOffset, heapOffset + 256));

	D3D12_CONSTANT_BUFFER_VIEW_DESC constantBufferViewDesc = {};
	constantBufferViewDesc.BufferLocation = m_constantsData.m_heap->GetGPUVirtualAddress() + heapOffset;
	constantBufferViewDesc.SizeInBytes = (UINT)256;
	m_device->CreateConstantBufferView(&constantBufferViewDesc,
		CD3DX12_CPU_DESCRIPTOR_HANDLE(getCurrentResourceStorage().m_scaleOffsetDescriptorHeap->GetCPUDescriptorHandleForHeapStart())
		.Offset((INT)getCurrentResourceStorage().m_currentScaleOffsetBufferIndex, g_descriptorStrideSRVCBVUAV));
}

void D3D12GSRender::FillVertexShaderConstantsBuffer()
{
	for (const auto& c : transform_constants)
	{
		size_t offset = c.first * 4 * sizeof(float);
		m_vertexConstants[offset] = c.second;
	}

	size_t bufferSize = 512 * 4 * sizeof(float);

	assert(m_constantsData.canAlloc(bufferSize));
	size_t heapOffset = m_constantsData.alloc(bufferSize);

	void *constantsBufferMap;
	ThrowIfFailed(m_constantsData.m_heap->Map(0, &CD3DX12_RANGE(heapOffset, heapOffset + bufferSize), &constantsBufferMap));
	for (const auto &vertexConstants : m_vertexConstants)
	{
		float data[4] = {
			vertexConstants.second.x,
			vertexConstants.second.y,
			vertexConstants.second.z,
			vertexConstants.second.w
		};
		streamToBuffer((char*)constantsBufferMap + heapOffset + vertexConstants.first, data, 4 * sizeof(float));
	}
	m_constantsData.m_heap->Unmap(0, &CD3DX12_RANGE(heapOffset, heapOffset + bufferSize));

	D3D12_CONSTANT_BUFFER_VIEW_DESC constantBufferViewDesc = {};
	constantBufferViewDesc.BufferLocation = m_constantsData.m_heap->GetGPUVirtualAddress() + heapOffset;
	constantBufferViewDesc.SizeInBytes = (UINT)bufferSize;
	m_device->CreateConstantBufferView(&constantBufferViewDesc,
		CD3DX12_CPU_DESCRIPTOR_HANDLE(getCurrentResourceStorage().m_constantsBufferDescriptorsHeap->GetCPUDescriptorHandleForHeapStart())
		.Offset((INT)getCurrentResourceStorage().m_constantsBufferIndex, g_descriptorStrideSRVCBVUAV));
}

void D3D12GSRender::FillPixelShaderConstantsBuffer()
{
	RSXFragmentProgram fragment_program;
	u32 shader_program = rsx::method_registers[NV4097_SET_SHADER_PROGRAM];
	fragment_program.offset = shader_program & ~0x3;
	fragment_program.addr = rsx::get_address(fragment_program.offset, (shader_program & 0x3) - 1);
	fragment_program.ctrl = rsx::method_registers[NV4097_SET_SHADER_CONTROL];
	// Get constant from fragment program
	const std::vector<size_t> &fragmentOffset = m_cachePSO.getFragmentConstantOffsetsCache(&fragment_program);
	size_t bufferSize = fragmentOffset.size() * 4 * sizeof(float) + 1;
	// Multiple of 256 never 0
	bufferSize = (bufferSize + 255) & ~255;

	assert(m_constantsData.canAlloc(bufferSize));
	size_t heapOffset = m_constantsData.alloc(bufferSize);

	size_t offset = 0;
	void *constantsBufferMap;
	ThrowIfFailed(m_constantsData.m_heap->Map(0, &CD3DX12_RANGE(heapOffset, heapOffset + bufferSize), &constantsBufferMap));
	for (size_t offsetInFP : fragmentOffset)
	{
		u32 vector[4];
		auto data = vm::get_ptr<be_t<u32>>(fragment_program.addr + offsetInFP);

		u32 c0 = (data[0] >> 16 | data[0] << 16);
		u32 c1 = (data[1] >> 16 | data[1] << 16);
		u32 c2 = (data[2] >> 16 | data[2] << 16);
		u32 c3 = (data[3] >> 16 | data[3] << 16);

		vector[0] = c0;
		vector[1] = c1;
		vector[2] = c2;
		vector[3] = c3;

		streamToBuffer((char*)constantsBufferMap + heapOffset + offset, vector, 4 * sizeof(u32));
		offset += 4 * sizeof(u32);
	}
	m_constantsData.m_heap->Unmap(0, &CD3DX12_RANGE(heapOffset, heapOffset + bufferSize));

	D3D12_CONSTANT_BUFFER_VIEW_DESC constantBufferViewDesc = {};
	constantBufferViewDesc.BufferLocation = m_constantsData.m_heap->GetGPUVirtualAddress() + heapOffset;
	constantBufferViewDesc.SizeInBytes = (UINT)bufferSize;
	m_device->CreateConstantBufferView(&constantBufferViewDesc,
		CD3DX12_CPU_DESCRIPTOR_HANDLE(getCurrentResourceStorage().m_constantsBufferDescriptorsHeap->GetCPUDescriptorHandleForHeapStart())
		.Offset((INT)getCurrentResourceStorage().m_constantsBufferIndex, g_descriptorStrideSRVCBVUAV));
}


#endif
