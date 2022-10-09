#include <objbase.h>
#include <d3dcompiler.h>
#include "D3d12GraphicsManager.h"
#include "WindowsApplication.h"

using namespace std;
namespace Corona
{
    extern IApplication* g_pApp;

    template<class T>
    inline void SafeRelease(T** ppInterfaceToRelease)
    {
        if(*ppInterfaceToRelease != nullptr)
        {
            (*ppInterfaceToRelease)->Release();

            (*ppInterfaceToRelease) = nullptr;
        }
    }

    static void GetHardwareAdapter(IDXGIFactory4* pFactory, IDXGIAdapter1** ppAdapter)
    {
        IDXGIAdapter1* pAdapter = nullptr;
        *ppAdapter = nullptr;

        for(UINT adapterIndex = 0; pFactory->EnumAdapters1(adapterIndex, &pAdapter) != DXGI_ERROR_NOT_FOUND; adapterIndex++)
        {
            DXGI_ADAPTER_DESC1 desc;
            pAdapter->GetDesc1(&desc);

            if(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            {
                // Don't select the Basic Render Driver adapter.
                continue;
            }

            // Check to see if the adapter supports Direct3D 12, but don't create the
            // actual device yet.
            if(SUCCEEDED(D3D12CreateDevice(pAdapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
            {
                break;
            }
        }

        *ppAdapter = pAdapter;
    }

    HRESULT D3d12GraphicsManager::WaitForPreviousFrame() {
        // WAITING FOR THE FRAME TO COMPLETE BEFORE CONTINUING IS NOT BEST PRACTICE.
        // This is code implemented as such for simplicity. More advanced samples 
        // illustrate how to use fences for efficient resource usage.
        
        // Signal and increment the fence value.
        HRESULT hr;
        const uint64_t fence = m_nFenceValue;
        if(FAILED(hr = m_pCommandQueue->Signal(m_pFence, fence)))
        {
            return hr;
        }

        m_nFenceValue++;

        // Wait until the previous frame is finished.
        if (m_pFence->GetCompletedValue() < fence)
        {
            if(FAILED(hr = m_pFence->SetEventOnCompletion(fence, m_hFenceEvent)))
            {
                return hr;
            }
            WaitForSingleObject(m_hFenceEvent, INFINITE);
        }

        m_nFrameIndex = m_pSwapChain->GetCurrentBackBufferIndex();

        return hr;
    }

    HRESULT D3d12GraphicsManager::CreateDescriptorHeaps() 
    {
        HRESULT hr;

        // Describe and create a render target view (RTV) descriptor heap.
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = kFrameCount;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if(FAILED(hr = m_pDev->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_pRtvHeap)))) {
            return hr;
        }

        m_nRtvDescriptorSize = m_pDev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        // Describe and create a depth stencil view (DSV) descriptor heap.
        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
        dsvHeapDesc.NumDescriptors = 1;
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if(FAILED(hr = m_pDev->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_pDsvHeap)))) {
            return hr;
        }

        // Describe and create a Shader Resource View (SRV) and 
        // Constant Buffer View (CBV) and 
        // Unordered Access View (UAV) descriptor heap.
        D3D12_DESCRIPTOR_HEAP_DESC cbvSrvUavHeapDesc = {};
        cbvSrvUavHeapDesc.NumDescriptors =
            kFrameCount * ( 1 + kMaxSceneObjectCount)           // 1 perFrame and kMaxSceneObjectCount perBatch Cbvs * kFrameCount
            + kMaxTextureCount;                                 // + kMaxTextureCount for the SRV(Texture).
        cbvSrvUavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        cbvSrvUavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if(FAILED(hr = m_pDev->CreateDescriptorHeap(&cbvSrvUavHeapDesc, IID_PPV_ARGS(&m_pCbvHeap)))) {
            return hr;
        }

        m_nCbvSrvDescriptorSize = m_pDev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // Describe and create a sampler descriptor heap.
        D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc = {};
        samplerHeapDesc.NumDescriptors = 2048; // this is the max D3d12 HW support currently
        samplerHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        samplerHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if(FAILED(hr = m_pDev->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&m_pSamplerHeap)))) {
            return hr;
        }

        if(FAILED(hr = m_pDev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_pCommandAllocator)))) {
            return hr;
        }

        return hr;
    }

    HRESULT D3d12GraphicsManager::CreateRenderTarget()
    {
        HRESULT hr;

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_pRtvHeap->GetCPUDescriptorHandleForHeapStart();

        // Create a RTV for each frame.
        for (uint32_t i = 0; i < kFrameCount; i++)
        {
            if (FAILED(hr = m_pSwapChain->GetBuffer(i, IID_PPV_ARGS(&m_pRenderTargets[i])))) {
                break;
            }
            m_pDev->CreateRenderTargetView(m_pRenderTargets[i], nullptr, rtvHandle);
            rtvHandle.ptr += m_nRtvDescriptorSize;
        }

        return hr;
    }

    HRESULT D3d12GraphicsManager::CreateDepthStencil()
    {
        HRESULT hr;

        // Create the depth stencil view.
        D3D12_DEPTH_STENCIL_VIEW_DESC depthStencilDesc = {};
        depthStencilDesc.Format = DXGI_FORMAT_D32_FLOAT;
        depthStencilDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        depthStencilDesc.Flags = D3D12_DSV_FLAG_NONE;

        D3D12_CLEAR_VALUE depthOptimizedClearValue = {};
        depthOptimizedClearValue.Format = DXGI_FORMAT_D32_FLOAT;
        depthOptimizedClearValue.DepthStencil.Depth = 1.0f;
        depthOptimizedClearValue.DepthStencil.Stencil = 0;

        D3D12_HEAP_PROPERTIES prop = {};
        prop.Type = D3D12_HEAP_TYPE_DEFAULT;
        prop.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        prop.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        prop.CreationNodeMask = 1;
        prop.VisibleNodeMask = 1;

        uint32_t width = g_pApp->GetConfiguration().screenWidth;
        uint32_t height = g_pApp->GetConfiguration().screenHeight;
        D3D12_RESOURCE_DESC resourceDesc = {};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resourceDesc.Alignment = 0;
        resourceDesc.Width = width;
        resourceDesc.Height = height;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 0;
        resourceDesc.Format = DXGI_FORMAT_D32_FLOAT;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.SampleDesc.Quality = 0;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        if (FAILED(hr = m_pDev->CreateCommittedResource(
            &prop,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &depthOptimizedClearValue,
            IID_PPV_ARGS(&m_pDepthStencilBuffer)
            ))) {
            return hr;
        }

        m_pDev->CreateDepthStencilView(m_pDepthStencilBuffer, &depthStencilDesc, m_pDsvHeap->GetCPUDescriptorHandleForHeapStart());

        return hr;
    }

    HRESULT D3d12GraphicsManager::CreateVertexBuffer(std::vector<VertexBasicAttribs>& v_property_array)
    {
        HRESULT hr;

        ID3D12Resource* pVertexBufferUploadHeap;

        // create vertex GPU heap 
        D3D12_HEAP_PROPERTIES prop = {};
        prop.Type = D3D12_HEAP_TYPE_DEFAULT;
        prop.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        prop.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        prop.CreationNodeMask = 1;
        prop.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC resourceDesc = {};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resourceDesc.Alignment = 0;
        // size in byte of resource
        // TODO
        resourceDesc.Width = v_property_array.size() * 11 * 4;
        resourceDesc.Height = 1;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.SampleDesc.Quality = 0;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        ID3D12Resource* pVertexBuffer;

        if (FAILED(hr = m_pDev->CreateCommittedResource(
            &prop,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&pVertexBuffer)
        )))
        {
            return hr;
        }

        prop.Type = D3D12_HEAP_TYPE_UPLOAD;
        prop.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        prop.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        prop.CreationNodeMask = 1;
        prop.VisibleNodeMask = 1;

        if (FAILED(hr = m_pDev->CreateCommittedResource(
            &prop,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&pVertexBufferUploadHeap)
        )))
        {
            return hr;
        }

        D3D12_SUBRESOURCE_DATA vertexData = {};
        vertexData.pData = v_property_array.data();

        UpdateSubresources<1>(m_pCommandList, pVertexBuffer, pVertexBufferUploadHeap, 0, 0, 1, &vertexData);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = pVertexBuffer;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_pCommandList->ResourceBarrier(1, &barrier);

        // initialize the vertex buffer view
        D3D12_VERTEX_BUFFER_VIEW vertexBufferView;
        vertexBufferView.BufferLocation = pVertexBuffer->GetGPUVirtualAddress();
        // TODO: automatically calculate stride and size
        vertexBufferView.StrideInBytes = 11 * 4;
        vertexBufferView.SizeInBytes = (UINT)v_property_array.size() * 11 * 4;
        m_VertexBufferView.push_back(vertexBufferView);

        m_Buffers.push_back(pVertexBuffer);
        m_Buffers.push_back(pVertexBufferUploadHeap);

        return hr;
    }

    HRESULT D3d12GraphicsManager::CreateIndexBuffer(std::vector<uint32_t>& index_array)
    {
        HRESULT hr;

        ID3D12Resource* pIndexBufferUploadHeap;

        // create index GPU heap
        D3D12_HEAP_PROPERTIES prop = {};
        prop.Type = D3D12_HEAP_TYPE_DEFAULT;
        prop.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        prop.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        prop.CreationNodeMask = 1;
        prop.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC resourceDesc = {};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resourceDesc.Alignment = 0;
        // ?
        resourceDesc.Width = index_array.size() * 4;
        resourceDesc.Height = 1;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.SampleDesc.Quality = 0;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        ID3D12Resource* pIndexBuffer;

        if (FAILED(hr = m_pDev->CreateCommittedResource(
            &prop,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&pIndexBuffer)
        )))
        {
            return hr;
        }

        prop.Type = D3D12_HEAP_TYPE_UPLOAD;

        if (FAILED(hr = m_pDev->CreateCommittedResource(
            &prop,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&pIndexBufferUploadHeap)
        )))
        {
            return hr;
        }

        D3D12_SUBRESOURCE_DATA indexData = {};
        indexData.pData = index_array.data();
        
        UpdateSubresources<1>(m_pCommandList, pIndexBuffer, pIndexBufferUploadHeap, 0, 0, 1, &indexData);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = pIndexBuffer;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_INDEX_BUFFER;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_pCommandList->ResourceBarrier(1, &barrier);

        // initialize the index buffer view
        D3D12_INDEX_BUFFER_VIEW indexBufferView;
        indexBufferView.BufferLocation = pIndexBuffer->GetGPUVirtualAddress();
        indexBufferView.Format = DXGI_FORMAT_R32_UINT;
        indexBufferView.SizeInBytes = (UINT)index_array.size() * 4;
        m_IndexBufferView.push_back(indexBufferView);

        m_Buffers.push_back(pIndexBuffer);
        m_Buffers.push_back(pIndexBufferUploadHeap);

        DrawBatchContext dbc;
        dbc.count = (int32_t)index_array.size();
        m_DrawBatchContext.push_back(std::move(dbc));

        return hr;
    }

    HRESULT D3d12GraphicsManager::CreateTextureBuffer()
    {
        // HRESULT hr;

        // // Describe and create a Texture2D.
        // D3D12_HEAP_PROPERTIES prop = {};
        // prop.Type = D3D12_HEAP_TYPE_DEFAULT;
        // prop.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        // prop.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        // prop.CreationNodeMask = 1;
        // prop.VisibleNodeMask = 1;

        // D3D12_RESOURCE_DESC textureDesc = {};
        // textureDesc.MipLevels = 1;
        // textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        // textureDesc.Width = image.Width;
        // textureDesc.Height = image.Height;
        // textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        // textureDesc.DepthOrArraySize = 1;
        // textureDesc.SampleDesc.Count = 1;
        // textureDesc.SampleDesc.Quality = 0;
        // textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

        // hr = m_pDev->CreateCommittedResource(
        //     &prop,
        //     D3D12_HEAP_FLAG_NONE,
        //     &textureDesc,
        //     D3D12_RESOURCE_STATE_COPY_DEST,
        //     nullptr,
        //     IID_PPV_ARGS(&m_pTextureBuffer));

        // return hr;

        // ?
        HRESULT hr;

        // Describe and create a Texture2D.
        D3D12_HEAP_PROPERTIES prop = {};
        prop.Type = D3D12_HEAP_TYPE_DEFAULT;
        prop.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        prop.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        prop.CreationNodeMask = 1;
        prop.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC textureDesc = {};
        textureDesc.MipLevels = 1;
        textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        // ? why width and height 1 here
        textureDesc.Width = 1;
        textureDesc.Height = 1;
        textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        textureDesc.DepthOrArraySize = 1;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.SampleDesc.Quality = 0;
        textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

        if (FAILED(hr = m_pDev->CreateCommittedResource(
            &prop,
            D3D12_HEAP_FLAG_NONE,
            &textureDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            nullptr,
            IID_PPV_ARGS(&m_pTextureBuffer))))
        {
            return hr;
        }

        for (int32_t i = 0; i < kMaxTextureCount; i++)
        {
            // Describe and create a SRV for the texture.
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE srvHandle;
            srvHandle.ptr = m_pCbvHeap->GetCPUDescriptorHandleForHeapStart().ptr + (kTextureDescStartIndex + i) * m_nCbvSrvDescriptorSize;
            m_pDev->CreateShaderResourceView(m_pTextureBuffer, &srvDesc, srvHandle);
        }

        return hr;
    }

    HRESULT D3d12GraphicsManager::CreateSamplerBuffer()
    {
        // Describe and create a sampler.
        D3D12_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.MinLOD = 0;
        samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
        samplerDesc.MipLODBias = 0.0f;
        samplerDesc.MaxAnisotropy = 1;
        samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        m_pDev->CreateSampler(&samplerDesc, m_pSamplerHeap->GetCPUDescriptorHandleForHeapStart());

        return S_OK;
    }

    HRESULT D3d12GraphicsManager::CreateConstantBuffer()
    {
        HRESULT hr;

        D3D12_HEAP_PROPERTIES prop = { D3D12_HEAP_TYPE_UPLOAD, 
            D3D12_CPU_PAGE_PROPERTY_UNKNOWN, 
            D3D12_MEMORY_POOL_UNKNOWN,
            1,
            1 };

        D3D12_RESOURCE_DESC resourceDesc = {};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resourceDesc.Alignment = 0;
        resourceDesc.Width = kSizeConstantBufferPerFrame * kFrameCount;
        resourceDesc.Height = 1;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.SampleDesc.Quality = 0;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        ID3D12Resource* pConstantUploadBuffer;
        if(FAILED(hr = m_pDev->CreateCommittedResource(
            &prop,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&pConstantUploadBuffer))))
        {
            return hr;
        }

        for (uint32_t i = 0; i < kFrameCount; i++)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE cbvHandle;
            cbvHandle.ptr = m_pCbvHeap->GetCPUDescriptorHandleForHeapStart().ptr + i * (1 + kMaxSceneObjectCount) * m_nCbvSrvDescriptorSize;
            // Describe and create a per frame constant buffer view.
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
            // ? why BufferLocation not add
            cbvDesc.BufferLocation = pConstantUploadBuffer->GetGPUVirtualAddress();
            cbvDesc.SizeInBytes = kSizePerFrameConstantBuffer;
            m_pDev->CreateConstantBufferView(&cbvDesc, cbvHandle);

            for (uint32_t j = 0; j < kMaxSceneObjectCount; j++)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE cbvHandle2;
                cbvHandle2.ptr = cbvHandle.ptr + (j + 1) * m_nCbvSrvDescriptorSize;
                // Describe and create a per frame constant buffer view.
                cbvDesc.BufferLocation = pConstantUploadBuffer->GetGPUVirtualAddress() + kSizePerFrameConstantBuffer + j * kSizePerBatchConstantBuffer;
                cbvDesc.SizeInBytes = kSizePerBatchConstantBuffer;
                m_pDev->CreateConstantBufferView(&cbvDesc, cbvHandle2);
            }
        }

        D3D12_RANGE readRange = { 0, 0 };
        hr = pConstantUploadBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_pCbvDataBegin));

        m_Buffers.push_back(pConstantUploadBuffer);

        return hr;
    }

    // ?
    HRESULT D3d12GraphicsManager::CreateRootSignature()
    {
        HRESULT hr = S_OK;

        D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};

        // This is the highest version the sample supports. If CheckFeatureSupport succeeds, the HighestVersion returned will not be greater than this.
        featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;

        if (FAILED(m_pDev->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData))))
        {
            featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
        }

        D3D12_DESCRIPTOR_RANGE1 ranges[3] = {
            { D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 2, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC },
            { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0 },
            { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0,D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC }
        };

        D3D12_ROOT_PARAMETER1 rootParameters[3] = {
            { D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, { 1, &ranges[0] }, D3D12_SHADER_VISIBILITY_ALL },
            { D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, { 1, &ranges[1] }, D3D12_SHADER_VISIBILITY_PIXEL },
            { D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, { 1, &ranges[2] }, D3D12_SHADER_VISIBILITY_PIXEL }
        };

        // Allow input layout and deny uneccessary access to certain pipeline stages.
        D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        D3D12_ROOT_SIGNATURE_DESC1 rootSignatureDesc = {
                _countof(rootParameters), rootParameters, 0, nullptr, rootSignatureFlags
            };

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC versionedRootSignatureDesc = {
            D3D_ROOT_SIGNATURE_VERSION_1_1,
        };

        versionedRootSignatureDesc.Desc_1_1 = rootSignatureDesc;

        ID3DBlob* signature = nullptr;
        ID3DBlob* error = nullptr;
        if (SUCCEEDED(hr = D3D12SerializeVersionedRootSignature(&versionedRootSignatureDesc, &signature, &error)))
        {
            hr = m_pDev->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_pRootSignature));
        }

        SafeRelease(&signature);
        SafeRelease(&error);

        return hr;
    }

    // this is the function that loads and prepares the shaders
    HRESULT D3d12GraphicsManager::InitializeShader(const char* vsFilename, const char* fsFilename) {
        HRESULT hr = S_OK;

        // load the shaders
        Buffer vertexShader = g_pAssetLoader->SyncOpenAndReadBinary(vsFilename);
        Buffer pixelShader = g_pAssetLoader->SyncOpenAndReadBinary(fsFilename);

        D3D12_SHADER_BYTECODE vertexShaderByteCode;
        vertexShaderByteCode.pShaderBytecode = vertexShader.GetData();
        vertexShaderByteCode.BytecodeLength = vertexShader.GetDataSize();

        D3D12_SHADER_BYTECODE pixelShaderByteCode;
        pixelShaderByteCode.pShaderBytecode = pixelShader.GetData();
        pixelShaderByteCode.BytecodeLength = pixelShader.GetDataSize();

        // create the input layout object
        D3D12_INPUT_ELEMENT_DESC ied[] =
        {
            // Attention: you cannot change here without changing VertexBuffer and VertexBufferView (in function CreateVertexBuffer)
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        D3D12_RASTERIZER_DESC rsd = { D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_BACK, TRUE, D3D12_DEFAULT_DEPTH_BIAS, D3D12_DEFAULT_DEPTH_BIAS_CLAMP,
                                    TRUE, FALSE, FALSE, 0, D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF };
        const D3D12_RENDER_TARGET_BLEND_DESC defaultRenderTargetBlend = { FALSE, FALSE,
            D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
            D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
            D3D12_LOGIC_OP_NOOP,
            D3D12_COLOR_WRITE_ENABLE_ALL
        };

        D3D12_BLEND_DESC bld = { FALSE, FALSE,
                                                {
                                                defaultRenderTargetBlend,
                                                defaultRenderTargetBlend,
                                                defaultRenderTargetBlend,
                                                defaultRenderTargetBlend,
                                                defaultRenderTargetBlend,
                                                defaultRenderTargetBlend,
                                                defaultRenderTargetBlend,
                                                }
                                        };

        const D3D12_DEPTH_STENCILOP_DESC defaultStencilOp = { D3D12_STENCIL_OP_KEEP, 
            D3D12_STENCIL_OP_KEEP, 
            D3D12_STENCIL_OP_KEEP, 
            D3D12_COMPARISON_FUNC_ALWAYS };

        D3D12_DEPTH_STENCIL_DESC dsd = { TRUE, 
            D3D12_DEPTH_WRITE_MASK_ALL, 
            D3D12_COMPARISON_FUNC_LESS, 
            FALSE, 
            D3D12_DEFAULT_STENCIL_READ_MASK, 
            D3D12_DEFAULT_STENCIL_WRITE_MASK, 
            defaultStencilOp, defaultStencilOp };

        // describe and create the graphics pipeline state object (PSO)
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psod = {};
        psod.pRootSignature = m_pRootSignature;
        psod.VS             = vertexShaderByteCode;
        psod.PS             = pixelShaderByteCode;
        psod.BlendState     = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psod.SampleMask     = UINT_MAX;
        psod.RasterizerState= CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psod.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psod.InputLayout    = { ied, _countof(ied) };
        psod.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psod.NumRenderTargets = 1;
        psod.RTVFormats[0]  = DXGI_FORMAT_R8G8B8A8_UNORM;
        psod.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psod.SampleDesc.Count = 1;

        // ? TODO: always fail here
        if (FAILED(hr = m_pDev->CreateGraphicsPipelineState(&psod, IID_PPV_ARGS(&m_pPipelineState))))
        {
            return hr;
        }

        hr = m_pDev->CreateCommandList(0, 
                    D3D12_COMMAND_LIST_TYPE_DIRECT, 
                    m_pCommandAllocator, 
                    m_pPipelineState, 
                    IID_PPV_ARGS(&m_pCommandList));

        return hr;
    }

    HRESULT D3d12GraphicsManager::InitializeBuffers()
    {
        HRESULT hr = S_OK;

        if (FAILED(hr = CreateDepthStencil())) {
            return hr;
        }

        if (FAILED(hr = CreateConstantBuffer())) {
            return hr;
        }

        if (FAILED(hr = CreateTextureBuffer())) {
            return hr;
        }

        if (FAILED(hr = CreateSamplerBuffer())) {
            return hr;
        }

        auto& scene = g_pSceneManager->GetSceneForRendering();
        auto pGeometry = scene.GetFirstGeometry();
        int32_t n = 0;
        while(pGeometry)
        {
            // if (pGeometryNode->Visible())
            // {
            //     auto pGeometry = scene.GetGeometry(pGeometryNode->GetSceneObjectRef());
            //     assert(pGeometry);
            //     auto pMesh = pGeometry->GetMesh().lock();
            //     if(!pMesh) continue;
                
            //     // Set the number of vertex properties.
            //     auto vertexPropertiesCount = pMesh->GetVertexPropertiesCount();
                
            //     // Set the number of vertices in the vertex array.
            //     auto vertexCount = pMesh->GetVertexCount();

            //     Buffer buff;

            //     for (decltype(vertexPropertiesCount) i = 0; i < vertexPropertiesCount; i++)
            //     {
            //         const SceneObjectVertexArray& v_property_array = pMesh->GetVertexPropertyArray(i);

            //         CreateVertexBuffer(v_property_array);
            //     }

            //     auto indexGroupCount = pMesh->GetIndexGroupCount();

            //     for (decltype(indexGroupCount) i = 0; i < indexGroupCount; i++)
            //     {
            //         const SceneObjectIndexArray& index_array      = pMesh->GetIndexArray(i);

            //         CreateIndexBuffer(index_array);
            //     }

            //     SetPerBatchShaderParameters(n);
            //     n++;
            // }

            for (auto& pPrimitive : pGeometry->Primitives)
            {
                auto& m_VertexData = pPrimitive->VertexData;
                CreateVertexBuffer(m_VertexData);
                auto& m_IndexData = pPrimitive->IndexData;
                CreateIndexBuffer(m_IndexData);

				// SetPerBatchShaderParameters(n);
				// n++;
            }

            pGeometry = scene.GetNextGeometry();
       }

        if (SUCCEEDED(hr = m_pCommandList->Close()))
        {
            ID3D12CommandList* ppCommandLists[] = { m_pCommandList };
            m_pCommandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

            if (FAILED(hr = m_pDev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_pFence))))
            {
                return hr;
            }

            m_nFenceValue = 1;

            m_hFenceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
            if (m_hFenceEvent == NULL)
            {
                hr = HRESULT_FROM_WIN32(GetLastError());
                if (FAILED(hr))
                    return hr;
            }

            WaitForPreviousFrame();
        }

        return hr;
    }

    HRESULT D3d12GraphicsManager::CreateGraphicsResources()
    {
        HRESULT hr;

#if defined(_DEBUG)
        // Enable the D3D12 debug layer.
        {
            ID3D12Debug* pDebugController;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&pDebugController))))
            {
                pDebugController->EnableDebugLayer();
            }
            SafeRelease(&pDebugController);
        }
#endif

        IDXGIFactory4* pFactory;
        if (FAILED(hr = CreateDXGIFactory1(IID_PPV_ARGS(&pFactory))))
        {
            return hr;
        }

        IDXGIAdapter1* pHardwareAdapter;
        GetHardwareAdapter(pFactory, &pHardwareAdapter);

        if (FAILED(D3D12CreateDevice(pHardwareAdapter,
            D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_pDev))))
        {

            IDXGIAdapter* pWarpAdapter;
            if (FAILED(hr = pFactory->EnumWarpAdapter(IID_PPV_ARGS(&pWarpAdapter))))
            {
                SafeRelease(&pFactory);
                return hr;
            }

            if (FAILED(hr = D3D12CreateDevice(pWarpAdapter, D3D_FEATURE_LEVEL_11_0,
                IID_PPV_ARGS(&m_pDev))))
            {
                SafeRelease(&pFactory);
                return hr;
            }
        }


        HWND hWnd = reinterpret_cast<WindowsApplication*>(g_pApp)->GetMainWindow();

        // Describe and create the command queue.
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

        if (FAILED(hr = m_pDev->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_pCommandQueue)))) {
            SafeRelease(&pFactory);
            return hr;
        }

        // create a struct to hold information about the swap chain
        DXGI_SWAP_CHAIN_DESC1 scd;

        // clear out the struct for use
        ZeroMemory(&scd, sizeof(DXGI_SWAP_CHAIN_DESC1));

        // fill the swap chain description struct
        scd.Width = g_pApp->GetConfiguration().screenWidth;
        scd.Height = g_pApp->GetConfiguration().screenHeight;
        scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;     	        // use 32-bit color
        scd.Stereo = FALSE;
        scd.SampleDesc.Count = 1;                               // multi-samples can not be used when in SwapEffect sets to
                                                                // DXGI_SWAP_EFFECT_FLOP_DISCARD
        scd.SampleDesc.Quality = 0;                             // multi-samples can not be used when in SwapEffect sets to
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;      // how swap chain is to be used
        scd.BufferCount = kFrameCount;                          // back buffer count
        scd.Scaling = DXGI_SCALING_STRETCH;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;         // DXGI_SWAP_EFFECT_FLIP_DISCARD only supported after Win10
                                                                // use DXGI_SWAP_EFFECT_DISCARD on platforms early than Win10
        scd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;     // allow full-screen transition

        IDXGISwapChain1* pSwapChain;
        if (FAILED(hr = pFactory->CreateSwapChainForHwnd(
                    m_pCommandQueue,                            // Swap chain needs the queue so that it can force a flush on it
                    hWnd,
                    &scd,
                    NULL,
                    NULL,
                    &pSwapChain
                )))
        {
            SafeRelease(&pFactory);
            return hr;
        }

        m_pSwapChain = reinterpret_cast<IDXGISwapChain3*>(pSwapChain);

        m_nFrameIndex = m_pSwapChain->GetCurrentBackBufferIndex();

        cout << "Creating Descriptor Heaps ...";
        if (FAILED(hr = CreateDescriptorHeaps())) {
            return hr;
        }
        cout << "Done!" << endl;

        cout << "Creating Render Targets ...";
        if (FAILED(hr = CreateRenderTarget())) {
            return hr;
        }
        cout << "Done!" << endl;

        cout << "Creating Root Signatures ...";
        if (FAILED(hr = CreateRootSignature())) {
            return hr;
        }
        cout << "Done!" << endl;

        cout << "Loading Shaders ...";
        if (FAILED(hr = InitializeShader("Shaders/HLSL/default.vert.cso", "Shaders/HLSL/default.frag.cso"))) {
            return hr;
        }
        cout << "Done!" << endl;

        cout << "Initialize Buffers ...";
        if (FAILED(hr = InitializeBuffers())) {
            return hr;
        }
        cout << "Done!" << endl;
        return hr;

    }

    int D3d12GraphicsManager::Initialize()
    {
        int result = GraphicsManager::Initialize();

        if (!result)
        {
            const GfxConfiguration& config = g_pApp->GetConfiguration();
            m_ViewPort = { 0.0f, 0.0f, static_cast<float>(config.screenWidth), static_cast<float>(config.screenHeight), 0.0f, 1.0f };
            m_ScissorRect = { 0, 0, static_cast<LONG>(config.screenWidth), static_cast<LONG>(config.screenHeight) };
            result = static_cast<int>(CreateGraphicsResources());
        }

        return result;
    }

    void D3d12GraphicsManager::Finalize()
    {
        WaitForPreviousFrame();

        SafeRelease(&m_pFence);
        for (auto p : m_Buffers) {
            SafeRelease(&p);
        }
        m_Buffers.clear();
        SafeRelease(&m_pCommandList);
        SafeRelease(&m_pPipelineState);
        SafeRelease(&m_pRtvHeap);
        SafeRelease(&m_pDsvHeap);
        SafeRelease(&m_pCbvHeap);
        SafeRelease(&m_pSamplerHeap);
        SafeRelease(&m_pRootSignature);
        SafeRelease(&m_pCommandQueue);
        SafeRelease(&m_pCommandAllocator);
        SafeRelease(&m_pDepthStencilBuffer);
        SafeRelease(&m_pTextureBuffer);
        for (uint32_t i = 0; i < kFrameCount; i++) {
            SafeRelease(&m_pRenderTargets[i]);
        }
        SafeRelease(&m_pSwapChain);
        SafeRelease(&m_pDev);
    }

    void D3d12GraphicsManager::Clear()
    {
        HRESULT hr;
        // command list allocators can only be reset when the associated 
        // command lists have finished execution on the GPU; apps should use 
        // fences to determine GPU execution progress.
        if (FAILED(hr = m_pCommandAllocator->Reset()))
        {
            return;
        }

        // however, when ExecuteCommandList() is called on a particular command 
        // list, that command list can then be reset at any time and must be before 
        // re-recording.
        if (FAILED(hr = m_pCommandList->Reset(m_pCommandAllocator, m_pPipelineState)))
        {
            return;
        }

        // Indicate that the back buffer will be used as a render target.
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = m_pRenderTargets[m_nFrameIndex];
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_pCommandList->ResourceBarrier(1, &barrier);

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle;
        rtvHandle.ptr = m_pRtvHeap->GetCPUDescriptorHandleForHeapStart().ptr + m_nFrameIndex * m_nRtvDescriptorSize;
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle;
        dsvHandle = m_pDsvHeap->GetCPUDescriptorHandleForHeapStart();
        m_pCommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

        // clear the back buffer to a deep blue
        const FLOAT clearColor[] = { 0.690196097f, 0.768627524f, 0.870588303f, 1.000000000f };
        m_pCommandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        m_pCommandList->ClearDepthStencilView(m_pDsvHeap->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    }

    void D3d12GraphicsManager::Draw()
    {
        PopulateCommandList();

        RenderBuffers();

        WaitForPreviousFrame();
    }

    HRESULT D3d12GraphicsManager::PopulateCommandList()
    {
        HRESULT hr;

        // Set necessary state.
        m_pCommandList->SetGraphicsRootSignature(m_pRootSignature);

        ID3D12DescriptorHeap* ppHeaps[] = { m_pCbvHeap, m_pSamplerHeap };
        m_pCommandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

        // CBV Per Frame
        D3D12_GPU_DESCRIPTOR_HANDLE cbvSrvHandle[2];
        uint32_t nFrameResourceDescriptorOffset = m_nFrameIndex * (1 + kMaxSceneObjectCount);
        cbvSrvHandle[0].ptr = m_pCbvHeap->GetGPUDescriptorHandleForHeapStart().ptr + nFrameResourceDescriptorOffset * m_nCbvSrvDescriptorSize;

        // Sampler
        m_pCommandList->SetGraphicsRootDescriptorTable(1, m_pSamplerHeap->GetGPUDescriptorHandleForHeapStart());

        // SRV
        D3D12_GPU_DESCRIPTOR_HANDLE srvHandle;
        srvHandle.ptr = m_pCbvHeap->GetGPUDescriptorHandleForHeapStart().ptr + kTextureDescStartIndex * m_nCbvSrvDescriptorSize;
        m_pCommandList->SetGraphicsRootDescriptorTable(2, srvHandle);

        m_pCommandList->RSSetViewports(1, &m_ViewPort);
        m_pCommandList->RSSetScissorRects(1, &m_ScissorRect);
        m_pCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        SetPerFrameShaderParameters();

        // do 3D rendering on the back buffer here
        cbvSrvHandle[1].ptr = m_pCbvHeap->GetGPUDescriptorHandleForHeapStart().ptr + nFrameResourceDescriptorOffset * m_nCbvSrvDescriptorSize;
        int32_t i = 0;
        //for (auto dbc : m_DrawBatchContext)
        //{

        //    // CBV Per Batch
        //    cbvSrvHandle[1].ptr = m_nCbvSrvDescriptorSize;
        //    m_pCommandList->SetGraphicsRootDescriptorTable(0, cbvSrvHandle[0]);

        //    // select which vertex buffer(s) to use
        //    m_pCommandList->IASetVertexBuffers(0, 2, &m_VertexBufferView[i * 2]);
        //    // select which index buffer to use
        //    m_pCommandList->IASetIndexBuffer(&m_IndexBufferView[i]);

        //    // draw the vertex buffer to the back buffer
        //    m_pCommandList->DrawIndexedInstanced(dbc.count, 1, 0, 0, 0);
        //    i++;
        //}
         
		for (auto dbc : m_DrawBatchContext)
		{

		    // CBV Per Batch
		    cbvSrvHandle[1].ptr = m_nCbvSrvDescriptorSize;
		    m_pCommandList->SetGraphicsRootDescriptorTable(0, cbvSrvHandle[0]);

		    // select which vertex buffer(s) to use
		    m_pCommandList->IASetVertexBuffers(0, 1, &m_VertexBufferView[i]);
		    // select which index buffer to use
		    m_pCommandList->IASetIndexBuffer(&m_IndexBufferView[i]);

		    // draw the vertex buffer to the back buffer
		    m_pCommandList->DrawIndexedInstanced(dbc.count, 1, 0, 0, 0);
		    i++;
		}

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = m_pRenderTargets[m_nFrameIndex];
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_pCommandList->ResourceBarrier(1, &barrier);

        hr = m_pCommandList->Close();

        return hr;
    }

    HRESULT D3d12GraphicsManager::RenderBuffers()
    {
        HRESULT hr;

        // execute the command list
        ID3D12CommandList *ppCommandLists[] = { m_pCommandList };
        m_pCommandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

        // swap the back buffer and the front buffer
        hr = m_pSwapChain->Present(1, 0);

        return hr;
    }

    bool D3d12GraphicsManager::SetPerFrameShaderParameters()
    {
        memcpy(m_pCbvDataBegin + m_nFrameIndex * kSizeConstantBufferPerFrame, &m_DrawFrameContext, sizeof(m_DrawFrameContext));
        return true;
    }

    bool D3d12GraphicsManager::SetPerBatchShaderParameters(int32_t index)
    {
        memcpy(m_pCbvDataBegin + m_nFrameIndex * kSizeConstantBufferPerFrame + kSizePerFrameConstantBuffer + index * kSizePerFrameConstantBuffer, 
            &m_DrawBatchContext, sizeof(m_DrawBatchContext));
        return true;
    }


}