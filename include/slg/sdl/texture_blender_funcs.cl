#line 2 "texture_blender_funcs.cl"

/***************************************************************************
 * Copyright 1998-2013 by authors (see AUTHORS.txt)                        *
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

#ifndef TEXTURE_STACK_SIZE
#define TEXTURE_STACK_SIZE 16
#endif

//------------------------------------------------------------------------------
// Blender wood texture
//------------------------------------------------------------------------------

#if defined (PARAM_ENABLE_BLENDER_WOOD)

float BlenderWoodTexture_Evaluate(__global Texture *texture, __global HitPoint *hitPoint){
	const float3 P = TextureMapping3D_Map(&texture->blenderWood.mapping, hitPoint);
	float scale = 1.f;
	if(fabs(texture->blenderWood.noisesize) > 0.00001f) scale = (1.f/texture->blenderWood.noisesize);

	const BlenderWoodNoiseBase noise = texture->blenderWood.noisebasis2;
	float wood = 0.f;

	switch(texture->blenderWood.type) {
		default:
		case BANDS:
			if(noise == TEX_SIN) {
				wood = tex_sin((P.x + P.y + P.z) * 10.f);
			} else if(noise == TEX_SAW) {
				wood = tex_saw((P.x + P.y + P.z) * 10.f);
			} else {
				wood = tex_tri((P.x + P.y + P.z) * 10.f);
			}
			break;
		case RINGS:
			if(noise == TEX_SIN) {
				wood = tex_sin(sqrt(P.x*P.x + P.y*P.y + P.z*P.z) * 20.f);
			} else if(noise == TEX_SAW) {
				wood = tex_saw(sqrt(P.x*P.x + P.y*P.y + P.z*P.z) * 20.f);
			} else {
				wood = tex_tri(sqrt(P.x*P.x + P.y*P.y + P.z*P.z) * 20.f);
			}
			break;
		case BANDNOISE:			
			if(texture->blenderWood.hard)	
				wood = texture->blenderWood.turbulence * fabs(2.f * Noise3(scale*P) - 1.f);
			else
				wood = texture->blenderWood.turbulence * Noise3(scale*P);

			if(noise == TEX_SIN) {
				wood = tex_sin((P.x + P.y + P.z) * 10.f + wood);
			} else if(noise == TEX_SAW) {
				wood = tex_saw((P.x + P.y + P.z) * 10.f + wood);
			} else {
				wood = tex_tri((P.x + P.y + P.z) * 10.f + wood);
			}
			break;
		case RINGNOISE:
			if(texture->blenderWood.hard)	
				wood = texture->blenderWood.turbulence * fabs(2.f * Noise3(scale*P) - 1.f);
			else
				wood = texture->blenderWood.turbulence * Noise3(scale*P);

			if(noise == TEX_SIN) {
				wood = tex_sin(sqrt(P.x*P.x + P.y*P.y + P.z*P.z) * 20.f + wood);
			} else if(noise == TEX_SAW) {
				wood = tex_saw(sqrt(P.x*P.x + P.y*P.y + P.z*P.z) * 20.f + wood);
			} else {
				wood = tex_tri(sqrt(P.x*P.x + P.y*P.y + P.z*P.z) * 20.f + wood);
			}
			break;
	}
	wood = (wood - 0.5f) * texture->blenderWood.contrast + texture->blenderWood.bright - 0.5f;
	if(wood < 0.f) wood = 0.f;
	else if(wood > 1.f) wood = 1.f;

	return wood;
}

void BlenderWoodTexture_EvaluateFloat(__global Texture *texture, __global HitPoint *hitPoint,
		float texValues[TEXTURE_STACK_SIZE], uint *texValuesSize) {
	texValues[(*texValuesSize)++] = BlenderWoodTexture_Evaluate(texture, hitPoint);
}

void BlenderWoodTexture_EvaluateSpectrum(__global Texture *texture, __global HitPoint *hitPoint,
	float3 texValues[TEXTURE_STACK_SIZE], uint *texValuesSize) {
    float wood = BlenderWoodTexture_Evaluate(texture, hitPoint);

    texValues[(*texValuesSize)++] = (float3)(wood, wood, wood);
}

//------------------------------------------------------------------------------
// Blender clouds texture
//------------------------------------------------------------------------------

#if defined (PARAM_ENABLE_BLENDER_CLOUDS)

float BlenderCloudsTexture_Evaluate(__global Texture *texture, __global HitPoint *hitPoint){
	const float3 P = TextureMapping3D_Map(&texture->blenderClouds.mapping, hitPoint);
	float scale = 1.f;
	if(fabs(texture->blenderClouds.noisesize) > 0.00001f) scale = (1.f/texture->blenderClouds.noisesize);

	float clouds = Turbulence(scale*P, texture->blenderClouds.noisesize, texture->blenderClouds.noisedepth);

	clouds = (clouds - 0.5f) * texture->blenderClouds.contrast + texture->blenderClouds.bright - 0.5f;
	if(clouds < 0.f) clouds = 0.f;
	else if(clouds > 1.f) clouds = 1.f;

	return clouds;
}

void BlenderCloudsTexture_EvaluateFloat(__global Texture *texture, __global HitPoint *hitPoint,
		float texValues[TEXTURE_STACK_SIZE], uint *texValuesSize) {
	texValues[(*texValuesSize)++] = BlenderCloudsTexture_Evaluate(texture, hitPoint);
}

void BlenderCloudsTexture_EvaluateSpectrum(__global Texture *texture, __global HitPoint *hitPoint,
		float3 texValues[TEXTURE_STACK_SIZE], uint *texValuesSize) {
		float clouds = BlenderCloudsTexture_Evaluate(texture, hitPoint);
	texValues[(*texValuesSize)++] = (float3)(clouds, clouds, clouds);
}

void BlenderCloudsTexture_EvaluateDuDv(__global Texture *texture, __global HitPoint *hitPoint,
		float2 texValues[TEXTURE_STACK_SIZE], uint *texValuesSize) {
	texValues[(*texValuesSize)++] = (float2)(DUDV_VALUE, DUDV_VALUE);
}

#endif