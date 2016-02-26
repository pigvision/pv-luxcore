/***************************************************************************
 * Copyright 1998-2015 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxRender.                                       *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 * You may obtain a copy of the License at                                 *
 *                                                                         *
 *     http://www.apache.org/licenses/LICENSE-2.0                          *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

// Boundary Volume Hierarchy accelerator
// Based of "Efficiency Issues for Ray Tracing" by Brian Smits
// Available at http://www.cs.utah.edu/~bes/papers/fastRT/paper.html

#include <iostream>
#include <functional>
#include <algorithm>
#include <limits>
#include <boost/foreach.hpp>

#include "luxrays/accelerators/mbvhaccel.h"
#include "luxrays/core/utils.h"
#include "luxrays/core/context.h"
#include "luxrays/core/exttrianglemesh.h"
#ifdef LUXRAYS_DISABLE_OPENCL
#include "luxrays/core/intersectiondevice.h"
#else
#include "luxrays/core/oclintersectiondevice.h"
#include "luxrays/kernels/kernels.h"
#endif

namespace luxrays {

#if !defined(LUXRAYS_DISABLE_OPENCL)

class OpenCLMBVHKernels : public OpenCLKernels {
public:
	OpenCLMBVHKernels(OpenCLIntersectionDevice *dev, const u_int kernelCount, const MBVHAccel *ac) :
			OpenCLKernels(dev, kernelCount), mbvh(ac),
			uniqueLeafsTransformBuff(NULL), uniqueLeafsMotionSystemBuff(NULL),
			uniqueLeafsInterpolatedTransformBuff(NULL) {
		const Context *deviceContext = device->GetContext();
		const std::string &deviceName(device->GetName());
		cl::Context &oclContext = device->GetOpenCLContext();
		cl::Device &oclDevice = device->GetOpenCLDevice();

		size_t maxNodeCount = 0;
		u_int pageNodeCount = 0;
		if (mbvh->nRootNodes) {
			// Check the max. number of vertices I can store in a single page
			const size_t maxMemAlloc = device->GetDeviceDesc()->GetMaxMemoryAllocSize();

			//------------------------------------------------------------------
			// Allocate vertex buffers
			//------------------------------------------------------------------

			// Check how many pages I have to allocate
			const size_t maxVertCount = maxMemAlloc / sizeof(Point);
			std::vector<std::vector<u_int> > vertOffsetPerLeafMesh;
			vertOffsetPerLeafMesh.resize(mbvh->uniqueLeafs.size());
			u_int totalVertCount = 0;
			for (u_int i = 0; i < mbvh->uniqueLeafs.size(); ++i) {
				const BVHAccel *leaf = mbvh->uniqueLeafs[i];

				for (u_int j = 0; j < leaf->meshes.size(); ++j) {
					vertOffsetPerLeafMesh[i].push_back(totalVertCount);
					totalVertCount += leaf->meshes[j]->GetTotalVertexCount();
				}
			}
			// Allocate a temporary buffer for the copy of the BVH vertices
			const u_int pageVertCount = Min<size_t>(totalVertCount, maxVertCount);
			Point *tmpVerts = new Point[pageVertCount];
			u_int tmpVertIndex = 0;

			u_int currentLeafIndex = 0;
			u_int currentMeshIndex = 0;
			u_int currentMeshVertIndex = 0;

			while (currentLeafIndex < mbvh->uniqueLeafs.size()) {
				const u_int tmpLeftVertCount = pageVertCount - tmpVertIndex;

				// Check if there is enough space in the temporary buffer for all vertices
				const Mesh *currentMesh = mbvh->uniqueLeafs[currentLeafIndex]->meshes[currentMeshIndex];
				const u_int toCopy = currentMesh->GetTotalVertexCount() - currentMeshVertIndex;
				if (tmpLeftVertCount >= toCopy) {
					// There is enough space for all mesh vertices
					memcpy(&tmpVerts[tmpVertIndex], &(currentMesh->GetVertices()[currentMeshVertIndex]),
							sizeof(Point) * toCopy);
					tmpVertIndex += toCopy;

					// Move to the next mesh
					++currentMeshIndex;
					if (currentMeshIndex >= mbvh->uniqueLeafs[currentLeafIndex]->meshes.size()) {
						// Move to the next leaf
						++currentLeafIndex;
						currentMeshIndex = 0;
					}
					currentMeshVertIndex = 0;
				} else {
					// There isn't enough space for all mesh vertices. Fill the current buffer.
					memcpy(&tmpVerts[tmpVertIndex], &(currentMesh->GetVertices()[currentMeshVertIndex]),
							sizeof(Point) * tmpLeftVertCount);

					tmpVertIndex += tmpLeftVertCount;
					currentMeshVertIndex += tmpLeftVertCount;
				}

				if ((tmpVertIndex >= pageVertCount) || (currentLeafIndex >=  mbvh->uniqueLeafs.size())) {
					// The temporary buffer is full, send the data to the OpenCL device
					LR_LOG(deviceContext, "[OpenCL device::" << deviceName <<
							"] Vertices buffer size (Page " << vertsBuffs.size() <<", " <<
							tmpVertIndex << " vertices): " <<
							(sizeof(Point) * pageVertCount / 1024) <<
							"Kbytes");
					cl::Buffer *vb = new cl::Buffer(oclContext,
						CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
						sizeof(Point) * tmpVertIndex,
						(void *)&tmpVerts[0]);
					device->AllocMemory(vb->getInfo<CL_MEM_SIZE>());
					vertsBuffs.push_back(vb);
					if (vertsBuffs.size() > 8)
						throw std::runtime_error("Too many vertex pages required in OpenCLMBVHKernels()");

					tmpVertIndex = 0;
				}
			}
			delete[] tmpVerts;

			//------------------------------------------------------------------
			// Allocate BVH node buffers
			//------------------------------------------------------------------

			// Check how many pages I have to allocate
			maxNodeCount = maxMemAlloc / sizeof(luxrays::ocl::BVHAccelArrayNode);

			u_int totalNodeCount = mbvh->nRootNodes;
			for (u_int i = 0; i < mbvh->uniqueLeafs.size(); ++i)
				totalNodeCount += mbvh->uniqueLeafs[i]->nNodes;

			pageNodeCount = Min<size_t>(totalNodeCount, maxNodeCount);

			// Initialize the node offset vector
			std::vector<u_int> leafNodeOffset;
			u_int currentBVHNodeCount = mbvh->nRootNodes;
			for (u_int i = 0; i < mbvh->uniqueLeafs.size(); ++i) {
				const BVHAccel *leafBVH = mbvh->uniqueLeafs[i];
				leafNodeOffset.push_back(currentBVHNodeCount);
				currentBVHNodeCount += leafBVH->nNodes;
			}

			// Allocate a temporary buffer for the copy of the BVH nodes
			luxrays::ocl::BVHAccelArrayNode *tmpNodes = new luxrays::ocl::BVHAccelArrayNode[pageNodeCount];
			u_int tmpNodeIndex = 0;

			u_int currentNodeIndex = 0;
			currentLeafIndex = 0;
			const luxrays::ocl::BVHAccelArrayNode *currentNodes = mbvh->bvhRootTree;
			u_int currentNodesCount = mbvh->nRootNodes;

			while (currentLeafIndex < mbvh->uniqueLeafs.size()) {
				const u_int tmpLeftNodeCount = pageNodeCount - tmpNodeIndex;
				const bool isRootTree = (currentNodes == mbvh->bvhRootTree);
				const u_int leafIndex = currentLeafIndex;

				// Check if there is enough space in the temporary buffer for all nodes
				u_int copiedIndexStart, copiedIndexEnd;
				const u_int toCopy = currentNodesCount - currentNodeIndex;
				if (tmpLeftNodeCount >= toCopy) {
					// There is enough space for all nodes
					memcpy(&tmpNodes[tmpNodeIndex], &currentNodes[currentNodeIndex],
							sizeof(luxrays::ocl::BVHAccelArrayNode) * toCopy);
					copiedIndexStart = tmpNodeIndex;
					copiedIndexEnd = tmpNodeIndex + toCopy;

					tmpNodeIndex += toCopy;
					// Move to the next leaf tree
					if (isRootTree) {
						// Move from the root nodes to the first leaf node. currentLeafIndex
						// is already 0
					} else
						++currentLeafIndex;

					if (currentLeafIndex < mbvh->uniqueLeafs.size()) {
						currentNodes = mbvh->uniqueLeafs[currentLeafIndex]->bvhTree;
						currentNodesCount = mbvh->uniqueLeafs[currentLeafIndex]->nNodes;
						currentNodeIndex = 0;
					}
				} else {
					// There isn't enough space for all mesh vertices. Fill the current buffer.
					memcpy(&tmpNodes[tmpNodeIndex], &currentNodes[currentNodeIndex],
							sizeof(luxrays::ocl::BVHAccelArrayNode) * tmpLeftNodeCount);
					copiedIndexStart = tmpNodeIndex;
					copiedIndexEnd = tmpNodeIndex + tmpLeftNodeCount;

					tmpNodeIndex += tmpLeftNodeCount;
					currentNodeIndex += tmpLeftNodeCount;
				}

				// Update the vertex references
				for (u_int i = copiedIndexStart; i < copiedIndexEnd; ++i) {
					luxrays::ocl::BVHAccelArrayNode *node = &tmpNodes[i];

					if (isRootTree) {
						// I'm handling the root nodes
						if (BVHNodeData_IsLeaf(node->nodeData)) {
							const u_int nextNodeIndex = leafNodeOffset[node->bvhLeaf.leafIndex];
							const u_int nodePage = nextNodeIndex / maxNodeCount;
							const u_int nodeIndex = nextNodeIndex % maxNodeCount;
							// Encode the page in 30, 29, 28 bits of the node index (last
							// bit is used to encode if it is a leaf or not)
							node->bvhLeaf.leafIndex = (nodePage << 28) | nodeIndex;
						} else {
							const u_int nextNodeIndex = BVHNodeData_GetSkipIndex(node->nodeData);
							const u_int nodePage = nextNodeIndex / maxNodeCount;
							const u_int nodeIndex = nextNodeIndex % maxNodeCount;
							// Encode the page in 30, 29, 28 bits of the node index (last
							// bit is used to encode if it is a leaf or not)
							node->nodeData = (nodePage << 28) | nodeIndex;
						}
					} else {
						// I'm handling a leaf nodes
						if (BVHNodeData_IsLeaf(node->nodeData)) {
							// Update the vertex references
							for (u_int j = 0; j < 3; ++j) {
								const u_int vertIndex = node->triangleLeaf.v[j] + vertOffsetPerLeafMesh[leafIndex][node->triangleLeaf.meshIndex];
								const u_int vertexPage = vertIndex / maxVertCount;
								const u_int vertexIndex = vertIndex % maxVertCount;
								// Encode the page in the last 3 bits of the vertex index
								node->triangleLeaf.v[j] = (vertexPage << 29) | vertexIndex;
							}
						} else {
							const u_int nextNodeIndex = BVHNodeData_GetSkipIndex(node->nodeData) + leafNodeOffset[leafIndex];
							const u_int nodePage = nextNodeIndex / maxNodeCount;
							const u_int nodeIndex = nextNodeIndex % maxNodeCount;
							// Encode the page in 30, 29, 28 bits of the node index (last
							// bit is used to encode if it is a leaf or not)
							node->nodeData = (nodePage << 28) | nodeIndex;
						}
					}
				}

				if ((tmpNodeIndex >= pageNodeCount) || (currentLeafIndex >=  mbvh->uniqueLeafs.size())) {
					// The temporary buffer is full, send the data to the OpenCL device
					LR_LOG(deviceContext, "[OpenCL device::" << deviceName <<
						"] MBVH node buffer size (Page " << nodeBuffs.size() <<", " <<
						tmpNodeIndex << " nodes): " <<
						(sizeof(luxrays::ocl::BVHAccelArrayNode) * totalNodeCount / 1024) <<
						"Kbytes");
					cl::Buffer *nb = new cl::Buffer(oclContext,
						CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
						sizeof(luxrays::ocl::BVHAccelArrayNode) * tmpNodeIndex,
						(void *)&tmpNodes[0]);
					device->AllocMemory(nb->getInfo<CL_MEM_SIZE>());
					nodeBuffs.push_back(nb);
					if (nodeBuffs.size() > 8)
						throw std::runtime_error("Too many BVH node pages required in OpenCLMBVHKernels()");

					tmpNodeIndex = 0;
				}
			}
			delete[] tmpNodes;
		}

		//----------------------------------------------------------------------
		// Allocate leaf transformations
		//----------------------------------------------------------------------

		if (mbvh->uniqueLeafsTransform.size() > 0) {
			// Transform CPU data structures in OpenCL data structures
			vector<Matrix4x4> mats;
			BOOST_FOREACH(const Transform *t, mbvh->uniqueLeafsTransform)
				mats.push_back(t->mInv);

			// Allocate the transformation buffer
			LR_LOG(deviceContext, "[OpenCL device::" << deviceName <<
				"] Leaf transformations buffer size: " <<
				(sizeof(luxrays::ocl::Matrix4x4) * mats.size() / 1024) <<
				"Kbytes");
			uniqueLeafsTransformBuff = new cl::Buffer(oclContext,
				CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
				sizeof(luxrays::ocl::Matrix4x4) * mats.size(),
				(void *)&(mats[0]));
			device->AllocMemory(uniqueLeafsTransformBuff->getInfo<CL_MEM_SIZE>());
		}

		if (mbvh->uniqueLeafsMotionSystem.size() > 0) {
			// Transform CPU data structures in OpenCL data structures
			
			vector<luxrays::ocl::MotionSystem> motionSystems;
			vector<luxrays::ocl::InterpolatedTransform> interpolatedTransforms;
			BOOST_FOREACH(const MotionSystem *ms, mbvh->uniqueLeafsMotionSystem) {
				luxrays::ocl::MotionSystem oclMotionSystem;

				oclMotionSystem.interpolatedTransformFirstIndex = interpolatedTransforms.size();
				BOOST_FOREACH(const InterpolatedTransform &it, ms->interpolatedTransforms) {
					// Here, I assume that luxrays::ocl::InterpolatedTransform and
					// luxrays::InterpolatedTransform are the same
					interpolatedTransforms.push_back(*((const luxrays::ocl::InterpolatedTransform *)&it));
				}
				oclMotionSystem.interpolatedTransformLastIndex = interpolatedTransforms.size() - 1;
				
				motionSystems.push_back(oclMotionSystem);
			}

			// Allocate the motion system buffer
			LR_LOG(deviceContext, "[OpenCL device::" << deviceName <<
				"] Leaf motion systems buffer size: " <<
				(sizeof(luxrays::ocl::MotionSystem) * motionSystems.size() / 1024) <<
				"Kbytes");
			uniqueLeafsMotionSystemBuff = new cl::Buffer(oclContext,
				CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
				sizeof(luxrays::ocl::MotionSystem) * motionSystems.size(),
				(void *)&(motionSystems[0]));
			device->AllocMemory(uniqueLeafsMotionSystemBuff->getInfo<CL_MEM_SIZE>());

			// Allocate the InterpolatedTransform buffer
			LR_LOG(deviceContext, "[OpenCL device::" << deviceName <<
				"] Leaf interpolated transforms buffer size: " <<
				(sizeof(luxrays::ocl::InterpolatedTransform) * interpolatedTransforms.size() / 1024) <<
				"Kbytes");
			uniqueLeafsInterpolatedTransformBuff = new cl::Buffer(oclContext,
				CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
				sizeof(luxrays::ocl::InterpolatedTransform) * interpolatedTransforms.size(),
				(void *)&(interpolatedTransforms[0]));
			device->AllocMemory(uniqueLeafsInterpolatedTransformBuff->getInfo<CL_MEM_SIZE>());
		}

		//----------------------------------------------------------------------
		// Compile kernel sources
		//----------------------------------------------------------------------

		// Compile options
		std::stringstream opts;
		opts << " -D LUXRAYS_OPENCL_KERNEL"
				" -D PARAM_RAY_EPSILON_MIN=" << MachineEpsilon::GetMin() << "f"
				" -D PARAM_RAY_EPSILON_MAX=" << MachineEpsilon::GetMax() << "f";
		//LR_LOG(deviceContext, "[OpenCL device::" << deviceName << "] MBVH compile options: \n" << opts.str());

		std::stringstream kernelDefs;
		kernelDefs << "#define MBVH_VERTS_PAGE_COUNT " << vertsBuffs.size() << "\n"
				"#define MBVH_NODES_PAGE_COUNT " << nodeBuffs.size() << "\n";
		if (vertsBuffs.size() > 1) {
			// MBVH_NODES_PAGE_SIZE is used only if MBVH_VERTS_PAGE_COUNT > 1
			// I conditional define this value to avoid kernel recompilation
			kernelDefs << "#define MBVH_NODES_PAGE_SIZE " << pageNodeCount << "\n";
		}
		for (u_int i = 0; i < vertsBuffs.size(); ++i)
			kernelDefs << "#define MBVH_VERTS_PAGE" << i << " 1\n";
		for (u_int i = 0; i < nodeBuffs.size(); ++i)
			kernelDefs << "#define MBVH_NODES_PAGE" << i << " 1\n";
		if (uniqueLeafsTransformBuff)
			kernelDefs << "#define MBVH_HAS_TRANSFORMATIONS 1\n";
		if (uniqueLeafsMotionSystemBuff)
			kernelDefs << "#define MBVH_HAS_MOTIONSYSTEMS 1\n";
		//LR_LOG(deviceContext, "[OpenCL device::" << deviceName << "] MBVH kernel definitions: \n" << kernelDefs.str());

		intersectionKernelSource = kernelDefs.str() +
				luxrays::ocl::KernelSource_bvh_types +
				luxrays::ocl::KernelSource_mbvh;

		const std::string code(
			kernelDefs.str() +
			luxrays::ocl::KernelSource_luxrays_types +
			luxrays::ocl::KernelSource_epsilon_types +
			luxrays::ocl::KernelSource_epsilon_funcs +
			luxrays::ocl::KernelSource_point_types +
			luxrays::ocl::KernelSource_vector_types +
			luxrays::ocl::KernelSource_matrix4x4_types +
			luxrays::ocl::KernelSource_matrix4x4_funcs +
			luxrays::ocl::KernelSource_quaternion_types +
			luxrays::ocl::KernelSource_quaternion_funcs +
			luxrays::ocl::KernelSource_ray_types +
			luxrays::ocl::KernelSource_ray_funcs +
			luxrays::ocl::KernelSource_bbox_types +
			luxrays::ocl::KernelSource_bbox_funcs +
			luxrays::ocl::KernelSource_transform_types +
			luxrays::ocl::KernelSource_transform_funcs +
			luxrays::ocl::KernelSource_motionsystem_types +
			luxrays::ocl::KernelSource_motionsystem_funcs +
			luxrays::ocl::KernelSource_triangle_types +
			luxrays::ocl::KernelSource_triangle_funcs +
			luxrays::ocl::KernelSource_bvh_types +
			luxrays::ocl::KernelSource_mbvh);
		cl::Program::Sources source(1, std::make_pair(code.c_str(), code.length()));
		cl::Program program = cl::Program(oclContext, source);
		try {
			VECTOR_CLASS<cl::Device> buildDevice;
			buildDevice.push_back(oclDevice);
			program.build(buildDevice, opts.str().c_str());
		} catch (cl::Error &err) {
			cl::STRING_CLASS strError = program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(oclDevice);
			LR_LOG(deviceContext, "[OpenCL device::" << deviceName << "] MBVH compilation error:\n" << strError.c_str());

			throw err;
		}

		// Setup kernels
		for (u_int i = 0; i < kernelCount; ++i) {
			kernels[i] = new cl::Kernel(program, "Accelerator_Intersect_RayBuffer");

			if (device->GetDeviceDesc()->GetForceWorkGroupSize() > 0)
				workGroupSize = device->GetDeviceDesc()->GetForceWorkGroupSize();
			else {
				kernels[i]->getWorkGroupInfo<size_t>(oclDevice,
					CL_KERNEL_WORK_GROUP_SIZE, &workGroupSize);
				//LR_LOG(deviceContext, "[OpenCL device::" << deviceName <<
				//	"] MBVH kernel work group size: " << workGroupSize);
			}

			// Set arguments
			SetIntersectionKernelArgs(*(kernels[i]), 3);
		}
	}

	virtual ~OpenCLMBVHKernels() {
		BOOST_FOREACH(cl::Buffer *buf, vertsBuffs) {
			device->FreeMemory(buf->getInfo<CL_MEM_SIZE>());
			delete buf;
		}
		BOOST_FOREACH(cl::Buffer *buf, nodeBuffs) {
			device->FreeMemory(buf->getInfo<CL_MEM_SIZE>());
			delete buf;
		}
		if (uniqueLeafsTransformBuff) {
			device->FreeMemory(uniqueLeafsTransformBuff->getInfo<CL_MEM_SIZE>());
			delete uniqueLeafsTransformBuff;
		}
		if (uniqueLeafsMotionSystemBuff) {
			device->FreeMemory(uniqueLeafsMotionSystemBuff->getInfo<CL_MEM_SIZE>());
			delete uniqueLeafsMotionSystemBuff;
		}
		if (uniqueLeafsInterpolatedTransformBuff) {
			device->FreeMemory(uniqueLeafsInterpolatedTransformBuff->getInfo<CL_MEM_SIZE>());
			delete uniqueLeafsInterpolatedTransformBuff;
		}
	}

	virtual void Update(const DataSet *newDataSet);
	virtual void EnqueueRayBuffer(cl::CommandQueue &oclQueue, const u_int kernelIndex,
		cl::Buffer &rBuff, cl::Buffer &hBuff, const u_int rayCount,
		const VECTOR_CLASS<cl::Event> *events, cl::Event *event);

	virtual u_int SetIntersectionKernelArgs(cl::Kernel &kernel, const u_int argIndex);

	const MBVHAccel *mbvh;

	// BVH fields
	vector<cl::Buffer *> vertsBuffs;
	vector<cl::Buffer *> nodeBuffs;
	cl::Buffer *uniqueLeafsTransformBuff;
	cl::Buffer *uniqueLeafsMotionSystemBuff;
	cl::Buffer *uniqueLeafsInterpolatedTransformBuff;
};

void OpenCLMBVHKernels::Update(const DataSet *newDataSet) {
	const Context *deviceContext = device->GetContext();
	const std::string &deviceName = device->GetName();
	LR_LOG(deviceContext, "[OpenCL device::" << deviceName << "] Updating DataSet transformations");

	// Transform CPU data structures in OpenCL data structures
	vector<Matrix4x4> mats;
	mats.reserve(mbvh->uniqueLeafsTransform.size());
	BOOST_FOREACH(const Transform *t, mbvh->uniqueLeafsTransform)
		mats.push_back(t->mInv);

	device->GetOpenCLQueue().enqueueWriteBuffer(
		*uniqueLeafsTransformBuff,
		CL_TRUE,
		0,
		sizeof(luxrays::ocl::Matrix4x4) * mats.size(),
		&mats[0]);
}

void OpenCLMBVHKernels::EnqueueRayBuffer(cl::CommandQueue &oclQueue, const u_int kernelIndex,
		cl::Buffer &rBuff, cl::Buffer &hBuff, const u_int rayCount,
		const VECTOR_CLASS<cl::Event> *events, cl::Event *event) {
	kernels[kernelIndex]->setArg(0, rBuff);
	kernels[kernelIndex]->setArg(1, hBuff);
	kernels[kernelIndex]->setArg(2, rayCount);

	const u_int globalRange = RoundUp<u_int>(rayCount, workGroupSize);
	oclQueue.enqueueNDRangeKernel(*kernels[kernelIndex], cl::NullRange,
		cl::NDRange(globalRange), cl::NDRange(workGroupSize), events,
		event);
}

u_int OpenCLMBVHKernels::SetIntersectionKernelArgs(cl::Kernel &kernel, const u_int index) {
	u_int argIndex = index;
	if (uniqueLeafsTransformBuff)
		kernel.setArg(argIndex++, *uniqueLeafsTransformBuff);
	if (uniqueLeafsMotionSystemBuff) {
		kernel.setArg(argIndex++, *uniqueLeafsMotionSystemBuff);
		kernel.setArg(argIndex++, *uniqueLeafsInterpolatedTransformBuff);
	}
	for (u_int i = 0; i < vertsBuffs.size(); ++i)
		kernel.setArg(argIndex++, *vertsBuffs[i]);
	for (u_int i = 0; i < nodeBuffs.size(); ++i)
		kernel.setArg(argIndex++, *nodeBuffs[i]);

	return argIndex;
}

OpenCLKernels *MBVHAccel::NewOpenCLKernels(OpenCLIntersectionDevice *device,
		const u_int kernelCount, const u_int stackSize) const {
	// Setup kernels
	return new OpenCLMBVHKernels(device, kernelCount, this);
}

#else

OpenCLKernels *MBVHAccel::NewOpenCLKernels(OpenCLIntersectionDevice *device,
		const u_int kernelCount, const u_int stackSize) const {
	return NULL;
}

#endif

}