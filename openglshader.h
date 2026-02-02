/**
 * @file openglshader.h
 * Shader definitions for OpenGL osd class
 *
 * @note This code was originally authored by Stefan Braun (see README),
 * but there was never set any copyright info.
 * @copyright (c) 2025 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPLv3
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.}
 */

#ifndef __SOFTHDDEVICE_OPENGLSHADER_H
#define __SOFTHDDEVICE_OPENGLSHADER_H

const char *rectVertexShader =
"#version 100 \n\
\
attribute vec2 position; \
varying vec4 rectCol; \
uniform vec4 inColor; \
uniform mat4 projection; \
\
void main() \
{ \
	gl_Position = projection * vec4(position.x, position.y, 0.0, 1.0); \
	rectCol = inColor; \
} \
";

const char *rectFragmentShader =
"#version 100 \n\
precision mediump float; \
varying vec4 rectCol; \
\
void main() \
{ \
	gl_FragColor = rectCol; \
} \
";

const char *textureVertexShader =
"#version 100 \n\
\
attribute vec2 position; \
attribute vec2 texCoords; \
\
varying vec2 TexCoords; \
varying vec4 alphaValue;\
varying vec4 bColorValue;\
\
uniform vec4 bColor; \
uniform mat4 projection; \
uniform vec4 alpha; \
\
void main() \
{ \
	gl_Position = projection * vec4(position.x, position.y, 0.0, 1.0); \
	TexCoords = texCoords; \
	alphaValue = alpha; \
	bColorValue = bColor; \
} \
";

const char *textureFragmentShader =
"#version 100 \n\
precision mediump float; \
varying vec2 TexCoords; \
varying vec4 alphaValue; \
varying vec4 bColorValue; \
\
uniform sampler2D screenTexture; \
\
float clamp_to_border_factor (vec2 coords) \
{ \
	bvec2 out1 = greaterThan (coords, vec2 (1,1)); \
	bvec2 out2 = lessThan (coords, vec2 (0,0)); \
	bool do_clamp = (any (out1) || any (out2)); \
	return float (!do_clamp); \
} \
\
void main() \
{ \
	vec4 color = texture2D(screenTexture, TexCoords) * alphaValue; \
	float f = clamp_to_border_factor (TexCoords); \
	gl_FragColor = mix (bColorValue, color, f); \
} \
";

const char *textureFragmentShaderSwapBR =
"#version 100 \n\
precision mediump float; \
varying vec2 TexCoords; \
varying vec4 alphaValue; \
varying vec4 bColorValue; \
\
uniform sampler2D screenTexture; \
\
float clamp_to_border_factor (vec2 coords) \
{ \
	bvec2 out1 = greaterThan (coords, vec2 (1,1)); \
	bvec2 out2 = lessThan (coords, vec2 (0,0)); \
	bool do_clamp = (any (out1) || any (out2)); \
	return float (!do_clamp); \
} \
\
void main() \
{ \
	vec4 color = texture2D(screenTexture, TexCoords) * alphaValue; \
	vec4 color_swapped = vec4(color.b, color.g, color.r, color.a); \
	float f = clamp_to_border_factor (TexCoords); \
	gl_FragColor = mix (bColorValue, color_swapped, f); \
} \
";

const char *textVertexShader =
"#version 100 \n\
\
attribute vec2 position; \
attribute vec2 texCoords; \
\
varying vec2 TexCoords; \
varying vec4 textColor; \
\
uniform mat4 projection; \
uniform vec4 inColor; \
\
void main() \
{ \
	gl_Position = projection * vec4(position.x, position.y, 0.0, 1.0); \
	TexCoords = texCoords; \
	textColor = inColor; \
} \
";

const char *textFragmentShader =
"#version 100 \n\
precision mediump float; \
varying vec2 TexCoords; \
varying vec4 textColor; \
\
uniform sampler2D glyphTexture; \
\
void main() \
{  \
	vec4 sampled = vec4(1.0, 1.0, 1.0, texture2D(glyphTexture, TexCoords).r); \
	gl_FragColor = textColor * sampled; \
} \
";

#endif
