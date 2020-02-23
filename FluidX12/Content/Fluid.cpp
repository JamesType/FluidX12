//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "Fluid.h"

using namespace std;
using namespace DirectX;
using namespace XUSG;

struct CBPerObject
{
	XMVECTOR LocalSpaceLightPt;
	XMVECTOR LocalSpaceEyePt;
	XMMATRIX ScreenToLocal;
	XMMATRIX WorldViewProj;
};

Fluid::Fluid(const Device& device) :
	m_device(device),
	m_frameParity(0)
{
	m_graphicsPipelineCache.SetDevice(device);
	m_computePipelineCache.SetDevice(device);
	m_pipelineLayoutCache.SetDevice(device);
}

Fluid::~Fluid()
{
}

bool Fluid::Init(const CommandList& commandList, uint32_t width, uint32_t height,
	shared_ptr<DescriptorTableCache> descriptorTableCache, vector<Resource>& uploaders,
	Format rtFormat, const XMUINT3& dim)
{
	m_descriptorTableCache = descriptorTableCache;
	m_gridSize = dim;
	m_viewport = XMUINT2(width, height);

	// Create resources
	for (auto i = 0ui8; i < 2; ++i)
	{
		N_RETURN(m_velocities[i].Create(m_device, dim.x, dim.y, dim.z, Format::R16G16B16A16_FLOAT,
			ResourceFlag::ALLOW_UNORDERED_ACCESS | ResourceFlag::ALLOW_SIMULTANEOUS_ACCESS,
			1, MemoryType::DEFAULT, (L"Velocity" + to_wstring(i)).c_str()), false);

		N_RETURN(m_dyes[i].Create(m_device, dim.x, dim.y, dim.z, Format::R16G16B16A16_FLOAT,
			ResourceFlag::ALLOW_UNORDERED_ACCESS, 1, MemoryType::DEFAULT,
			(L"Dye" + to_wstring(i)).c_str()), false);
	}

	N_RETURN(m_incompress.Create(m_device, dim.x, dim.y, dim.z, Format::R32_FLOAT,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, 1, MemoryType::DEFAULT,
		L"Incompressibility"), false);

	ResourceBarrier barrier;
	const auto numBarriers = m_incompress.SetBarrier(&barrier, ResourceState::UNORDERED_ACCESS);
	commandList.Barrier(numBarriers, &barrier);

	// Create pipelines
	N_RETURN(createPipelineLayouts(), false);
	N_RETURN(createPipelines(rtFormat), false);
	N_RETURN(createDescriptorTables(), false);

	return true;
}

void Fluid::UpdateFrame(float timeStep, CXMMATRIX viewProj, const XMFLOAT3& eyePt)
{
	m_timeStep = timeStep;
	m_frameParity = !m_frameParity;
	XMStoreFloat4x4(&m_viewProj, XMMatrixTranspose(viewProj));
	m_eyePt = eyePt;
}

void Fluid::Simulate(const CommandList& commandList)
{
	ResourceBarrier barriers[3];

	// Advection
	{
		// Set barriers (promotions)
		m_velocities[0].SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE);
		auto numBarriers = m_velocities[1].SetBarrier(barriers, ResourceState::UNORDERED_ACCESS);
		numBarriers = m_dyes[m_frameParity].SetBarrier(barriers, ResourceState::UNORDERED_ACCESS, numBarriers);
		commandList.Barrier(numBarriers, barriers);

		// Set pipeline state
		commandList.SetComputePipelineLayout(m_pipelineLayouts[ADVECT]);
		commandList.SetPipelineState(m_pipelines[ADVECT]);

		// Set descriptor tables
		commandList.SetCompute32BitConstant(0, reinterpret_cast<uint32_t&>(m_timeStep));
		commandList.SetComputeDescriptorTable(1, m_srvUavTables[SRV_UAV_TABLE_VECOLITY]);
		commandList.SetComputeDescriptorTable(2, m_srvUavTables[SRV_UAV_TABLE_DYE + m_frameParity]);
		commandList.SetComputeDescriptorTable(3, m_samplerTable);

		commandList.Dispatch(DIV_UP(m_gridSize.x, 8), DIV_UP(m_gridSize.y, 8), m_gridSize.z);
	}

	// Projection
	{
		// Set barriers
		auto numBarriers = m_velocities[0].SetBarrier(barriers, ResourceState::UNORDERED_ACCESS);
		numBarriers = m_velocities[1].SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers);
		numBarriers = m_dyes[m_frameParity].SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE |
			ResourceState::PIXEL_SHADER_RESOURCE, numBarriers);
		commandList.Barrier(numBarriers, barriers);

		// Set pipeline state
		commandList.SetComputePipelineLayout(m_pipelineLayouts[PROJECT]);
		commandList.SetPipelineState(m_pipelines[PROJECT]);

		// Set descriptor tables
		commandList.SetComputeDescriptorTable(0, m_srvUavTables[SRV_UAV_TABLE_VECOLITY1]);
		
		XMUINT3 numGroups;
		if (m_gridSize.z > 1) // optimized for 3D
		{
			numGroups.x = DIV_UP(m_gridSize.x, 4);
			numGroups.y = DIV_UP(m_gridSize.y, 4);
			numGroups.z = DIV_UP(m_gridSize.z, 4);
		}
		else
		{
			numGroups.x = DIV_UP(m_gridSize.x, 8);
			numGroups.y = DIV_UP(m_gridSize.y, 8);
			numGroups.z = m_gridSize.z;
		}

		commandList.Dispatch(numGroups.x, numGroups.y, numGroups.z);
	}
}

void Fluid::Render(const CommandList& commandList)
{
	if (m_gridSize.z > 1) rayCast(commandList);
	else visualizeDye(commandList);
}

bool Fluid::createPipelineLayouts()
{
	// Advection
	{
		Util::PipelineLayout pipelineLayout;
		pipelineLayout.SetConstants(0, SizeOfInUint32(float), 0);
		pipelineLayout.SetRange(1, DescriptorType::SRV, 1, 0);
		pipelineLayout.SetRange(1, DescriptorType::UAV, 1, 0);
		pipelineLayout.SetRange(2, DescriptorType::SRV, 1, 1);
		pipelineLayout.SetRange(2, DescriptorType::UAV, 1, 1);
		pipelineLayout.SetRange(3, DescriptorType::SAMPLER, 1, 0);
		X_RETURN(m_pipelineLayouts[ADVECT], pipelineLayout.GetPipelineLayout(m_pipelineLayoutCache,
			PipelineLayoutFlag::NONE, L"AdvectionLayout"), false);
	}

	// Projection
	{
		Util::PipelineLayout pipelineLayout;
		pipelineLayout.SetRange(0, DescriptorType::SRV, 1, 0);
		pipelineLayout.SetRange(0, DescriptorType::UAV, 2, 0);
		X_RETURN(m_pipelineLayouts[PROJECT], pipelineLayout.GetPipelineLayout(m_pipelineLayoutCache,
			PipelineLayoutFlag::NONE, L"ProjectionLayout"), false);
	}

	if (m_gridSize.z > 1)
	{
		// Ray casting
		Util::PipelineLayout pipelineLayout;
		pipelineLayout.SetConstants(0, SizeOfInUint32(CBPerObject), 0, 0, Shader::Stage::PS);
		pipelineLayout.SetRange(1, DescriptorType::SRV, 1, 0);
		pipelineLayout.SetRange(2, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout.SetShaderStage(0, Shader::Stage::PS);
		pipelineLayout.SetShaderStage(1, Shader::Stage::PS);
		pipelineLayout.SetShaderStage(2, Shader::Stage::PS);
		X_RETURN(m_pipelineLayouts[VISUALIZE], pipelineLayout.GetPipelineLayout(m_pipelineLayoutCache,
			PipelineLayoutFlag::NONE, L"RayCastingLayout"), false);
	}
	else
	{
		// Dye visualization
		Util::PipelineLayout pipelineLayout;
		pipelineLayout.SetRange(0, DescriptorType::SRV, 1, 0);
		pipelineLayout.SetRange(1, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout.SetShaderStage(0, Shader::PS);
		pipelineLayout.SetShaderStage(1, Shader::PS);
		X_RETURN(m_pipelineLayouts[VISUALIZE], pipelineLayout.GetPipelineLayout(m_pipelineLayoutCache,
			PipelineLayoutFlag::NONE, L"DyeVisualizationLayout"), false);
	}

	return true;
}

bool Fluid::createPipelines(Format rtFormat)
{
	auto vsIndex = 0u;
	auto psIndex = 0u;
	auto csIndex = 0u;

	// Advection
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, csIndex, L"CSAdvect.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[ADVECT]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[ADVECT], state.GetPipeline(m_computePipelineCache, L"Advection"), false);
	}

	// Projection
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, csIndex, m_gridSize.z > 1 ?
			L"CSProject3D.cso" : L"CSProject2D.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[PROJECT]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[PROJECT], state.GetPipeline(m_computePipelineCache, L"Projection"), false);
	}

	// Visualization
	N_RETURN(m_shaderPool.CreateShader(Shader::Stage::VS, vsIndex, L"VSScreenQuad.cso"), false);
	if (m_gridSize.z > 1)
	{
		// Ray casting
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::PS, psIndex, L"PSRayCast.cso"), false);

		Graphics::State state;
		state.SetPipelineLayout(m_pipelineLayouts[VISUALIZE]);
		state.SetShader(Shader::Stage::VS, m_shaderPool.GetShader(Shader::Stage::VS, vsIndex++));
		state.SetShader(Shader::Stage::PS, m_shaderPool.GetShader(Shader::Stage::PS, psIndex));
		state.IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state.DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache);
		state.OMSetBlendState(Graphics::NON_PRE_MUL, m_graphicsPipelineCache);
		state.OMSetRTVFormats(&rtFormat, 1);
		X_RETURN(m_pipelines[VISUALIZE], state.GetPipeline(m_graphicsPipelineCache, L"RayCasting"), false);
	}
	else
	{
		// Dye visualization
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::PS, psIndex, L"PSVisualizeDye.cso"), false);

		Graphics::State state;
		state.SetPipelineLayout(m_pipelineLayouts[VISUALIZE]);
		state.SetShader(Shader::Stage::VS, m_shaderPool.GetShader(Shader::Stage::VS, vsIndex));
		state.SetShader(Shader::Stage::PS, m_shaderPool.GetShader(Shader::Stage::PS, psIndex++));
		state.IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state.DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache);
		state.OMSetRTVFormats(&rtFormat, 1);
		X_RETURN(m_pipelines[VISUALIZE], state.GetPipeline(m_graphicsPipelineCache, L"DyeVisualization"), false);
	}

	return true;
}

bool Fluid::createDescriptorTables()
{
	// Create SRV and UAV tables
	for (auto i = 0ui8; i < 2; ++i)
	{
		{
			Util::DescriptorTable srvUavTable;
			const Descriptor descriptors[] =
			{
				m_dyes[(i + 1) % 2].GetSRV(),
				m_dyes[i].GetUAV()
			};
			srvUavTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
			X_RETURN(m_srvUavTables[SRV_UAV_TABLE_DYE + i], srvUavTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
		}

		{
			Util::DescriptorTable srvUavTable;
			const Descriptor descriptors[] =
			{
				m_velocities[i].GetSRV(),
				m_velocities[(i + 1) % 2].GetUAV()
			};
			srvUavTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
			X_RETURN(m_srvUavTables[SRV_UAV_TABLE_VECOLITY + i], srvUavTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
		}
	}

	// Create incompressibility UAV
	{
		Util::DescriptorTable uavTable;
		uavTable.SetDescriptors(0, 1, &m_incompress.GetUAV());
		X_RETURN(m_uavTable, uavTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Create the sampler
	{
		Util::DescriptorTable samplerTable;
		const auto samplerLinearClamp = SamplerPreset::LINEAR_CLAMP;
		samplerTable.SetSamplers(0, 1, &samplerLinearClamp, *m_descriptorTableCache);
		X_RETURN(m_samplerTable, samplerTable.GetSamplerTable(*m_descriptorTableCache), false);
	}

	return true;
}

void Fluid::visualizeDye(const CommandList& commandList)
{
	// Set pipeline state
	commandList.SetGraphicsPipelineLayout(m_pipelineLayouts[VISUALIZE]);
	commandList.SetPipelineState(m_pipelines[VISUALIZE]);

	commandList.IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);

	// Set descriptor tables
	commandList.SetGraphicsDescriptorTable(0, m_srvUavTables[SRV_UAV_TABLE_DYE + !m_frameParity]);
	commandList.SetGraphicsDescriptorTable(1, m_samplerTable);

	commandList.Draw(3, 1, 0, 0);
}

void Fluid::rayCast(const CommandList& commandList)
{
	// General matrices
	const auto world = XMMatrixScaling(8.0f, 8.0f, 8.0f);
	const auto worldI = XMMatrixInverse(nullptr, world);
	const auto worldViewProj = world * XMMatrixTranspose(XMLoadFloat4x4(&m_viewProj));

	// Screen space matrices
	CBPerObject cbPerObject;
	cbPerObject.LocalSpaceLightPt = XMVector3TransformCoord(XMVectorSet(75.0f, 75.0f, -75.0f, 0.0f), worldI);
	cbPerObject.LocalSpaceEyePt = XMVector3TransformCoord(XMLoadFloat3(&m_eyePt), worldI);

	const auto mToScreen = XMMATRIX
	(
		0.5f * m_viewport.x, 0.0f, 0.0f, 0.0f,
		0.0f, -0.5f * m_viewport.y, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.5f * m_viewport.x, 0.5f * m_viewport.y, 0.0f, 1.0f
	);
	const auto localToScreen = XMMatrixMultiply(worldViewProj, mToScreen);
	const auto screenToLocal = XMMatrixInverse(nullptr, localToScreen);
	cbPerObject.ScreenToLocal = XMMatrixTranspose(screenToLocal);
	cbPerObject.WorldViewProj = XMMatrixTranspose(worldViewProj);

	// Set pipeline state
	commandList.SetGraphicsPipelineLayout(m_pipelineLayouts[VISUALIZE]);
	commandList.SetPipelineState(m_pipelines[VISUALIZE]);

	commandList.IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);

	// Set descriptor tables
	commandList.SetGraphics32BitConstants(0, SizeOfInUint32(cbPerObject), &cbPerObject);
	commandList.SetGraphicsDescriptorTable(1, m_srvUavTables[SRV_UAV_TABLE_DYE + !m_frameParity]);
	commandList.SetGraphicsDescriptorTable(2, m_samplerTable);

	commandList.Draw(3, 1, 0, 0);
}
