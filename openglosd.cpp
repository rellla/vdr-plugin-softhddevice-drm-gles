/**
 * @file openglosd.cpp
 * Osd class - hardware accelerated (OpenGL/ES)
 *
 * This file defines cOglOsd and all other osd classes, which
 * create and handle the OpenGL accelerated OSD.
 *
 * @note This file was originally authored by Stefan Braun (see README),
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

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <vector>

#ifdef GRIDPOINTS
#include <string>
#endif

#ifdef WRITE_PNG
#include <png.h>
#endif

#include <sys/ioctl.h>

#include <GLES2/gl2.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <vdr/osd.h>

#include "logger.h"
#include "misc.h"
#include "openglosd.h"
#include "openglshader.h"
#include "softhddevice.h"
#include "videorender.h"

// This is needed for the GLES2 GL_CLAMP_TO_BORDER workaround
#define BORDERCOLOR         0x00000000

// This maybe useful for skin developing
#ifdef GRIDPOINTS
#define GRIDPOINTSTEXT      1
#define GRIDRECT            1
#define GRIDTEXT            0
#define GRIDPOINTSIZE       3
#define GRIDPOINTOFFSET     4
#define GRIDPOINTSTXTSIZE   14
#define GRIDPOINTBG         clrTransparent
#define GRIDPOINTCLR        0xFFFF0000
#endif

/****************************************************************************************
* Helpers
****************************************************************************************/
#ifdef WRITE_PNG
static int writeImage(char* filename, int width, int height, void *buffer, char* title)
{
	int code;
	FILE *fp;
	png_structp png_ptr;
	png_infop info_ptr;

	// Open file for writing (binary mode)
	fp = fopen(filename, "wb");
	if (fp == NULL) {
		LOGERROR("WritePng: Could not open file %s for writing", filename);
		code = 1;
		goto finalise;
	}

	// Initialize write structure
	png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (png_ptr == NULL) {
		LOGERROR("WritePng: Could not allocate write struct");
		code = 1;
		goto finalise;
	}

	// Initialize info structure
	info_ptr = png_create_info_struct(png_ptr);
	if (info_ptr == NULL) {
		LOGERROR("WritePng: Could not allocate info struct");
		code = 1;
		goto finalise;
	}

	// Setup Exception handling
	if (setjmp(png_jmpbuf(png_ptr))) {
		LOGERROR("WritePng: Error during png creation");
		code = 1;
		goto finalise;
	}

	png_init_io(png_ptr, fp);

	// Write header (8 bit colour depth)
	png_set_IHDR(png_ptr, info_ptr, width, height,
		8, PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE,
		PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

	// Set title
	if (title != NULL) {
		png_text title_text;
		title_text.compression = PNG_TEXT_COMPRESSION_NONE;
		title_text.key = strdup("Title");
		title_text.text = title;
		png_set_text(png_ptr, info_ptr, &title_text, 1);
	}

	png_write_info(png_ptr, info_ptr);

	// Write image data
	int i;
	for (i = height - 1; i >= 0; i--) {
		png_write_row(png_ptr, (png_bytep)buffer + i * width * 4);
	}

	// End write
	png_write_end(png_ptr, NULL);

	code = 0;
finalise:
	if (fp != NULL) fclose(fp);
	if (info_ptr != NULL) png_free_data(png_ptr, info_ptr, PNG_FREE_ALL, -1);
	if (png_ptr != NULL) png_destroy_write_struct(&png_ptr, (png_infopp)NULL);

	return code;
}

static void writePng(int x, int y, int w, int h, bool oFb) {
	GL_CHECK(glFinish());
	GLubyte result[w * h * 4];
	static int scr_nr = 0;
	char filename[40];

	GLenum fbstatus;
	GL_CHECK(fbstatus = glCheckFramebufferStatus(GL_FRAMEBUFFER));
	if(fbstatus != GL_FRAMEBUFFER_COMPLETE)
		LOGERROR("WritePng: Framebuffer is not complete! %d", fbstatus);

	GL_CHECK(glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, &result));
	if (oFb) {
		snprintf(filename, sizeof(filename), "/tmp/%03doFb.png", scr_nr++);
	} else {
		snprintf(filename, sizeof(filename), "/tmp/%03dbFb.png", scr_nr++);
	}
	writeImage(filename, w, h, &result, strdup("osd"));
}
#endif

void ConvertColor(const GLint &colARGB, glm::vec4 &col) {
	col.a = ((colARGB & 0xFF000000) >> 24) / 255.0;
	col.r = ((colARGB & 0x00FF0000) >> 16) / 255.0;
	col.g = ((colARGB & 0x0000FF00) >> 8 ) / 255.0;
	col.b = ((colARGB & 0x000000FF)      ) / 255.0;
}

/****************************************************************************************
* cShader
****************************************************************************************/
static cShader *Shaders[stCount];

void cShader::Use(void)
{
	GL_CHECK(glUseProgram(m_id));
}

bool cShader::Load(eShaderType type)
{
	const char *vertexCode = NULL;
	const char *fragmentCode = NULL;

	m_type = type;
	switch (m_type) {
		case stRect:
			vertexCode = rectVertexShader;
			fragmentCode = rectFragmentShader;
			break;
		case stTexture:
			vertexCode = textureVertexShader;
			fragmentCode = textureFragmentShader;
			break;
		case stTextureSwapBR:
			vertexCode = textureVertexShader;
			fragmentCode = textureFragmentShaderSwapBR;
			break;
		case stText:
			vertexCode = textVertexShader;
			fragmentCode = textFragmentShader;
			break;
		default:
			LOGERROR("openglosd: %s: unknown shader type", __FUNCTION__);
			break;
	}

	if (vertexCode == NULL || fragmentCode == NULL) {
		LOGERROR("openglosd: %s: error reading shader", __FUNCTION__);
		return false;
	}

	if (!Compile(vertexCode, fragmentCode)) {
		LOGERROR("openglosd: %s: error compiling shader", __FUNCTION__);
		return false;
	}

	return true;
}

void cShader::SetFloat(const GLchar *name, GLfloat value)
{
	GL_CHECK(glUniform1f(glGetUniformLocation(m_id, name), value));
}

void cShader::SetInteger(const GLchar *name, GLint value)
{
	GL_CHECK(glUniform1i(glGetUniformLocation(m_id, name), value));
}

void cShader::SetVector2f(const GLchar *name, GLfloat x, GLfloat y)
{
	GL_CHECK(glUniform2f(glGetUniformLocation(m_id, name), x, y));
}

void cShader::SetVector3f(const GLchar *name, GLfloat x, GLfloat y, GLfloat z)
{
	GL_CHECK(glUniform3f(glGetUniformLocation(m_id, name), x, y, z));
}

void cShader::SetVector4f(const GLchar *name, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
	GL_CHECK(glUniform4f(glGetUniformLocation(m_id, name), x, y, z, w));
}

void cShader::SetMatrix4(const GLchar *name, const glm::mat4 &matrix)
{
	GL_CHECK(glUniformMatrix4fv(glGetUniformLocation(m_id, name), 1, GL_FALSE, glm::value_ptr(matrix)));
}

bool cShader::Compile(const char *vertexCode, const char *fragmentCode)
{
	GLuint sVertex, sFragment;

	// vertex shader
	GL_CHECK(sVertex = glCreateShader(GL_VERTEX_SHADER));
	GL_CHECK(glShaderSource(sVertex, 1, &vertexCode, NULL));
	GL_CHECK(glCompileShader(sVertex));
	if (!CheckCompileErrors(sVertex))
		return false;

	// fragment shader
	GL_CHECK(sFragment = glCreateShader(GL_FRAGMENT_SHADER));
	GL_CHECK(glShaderSource(sFragment, 1, &fragmentCode, NULL));
	GL_CHECK(glCompileShader(sFragment));
	if (!CheckCompileErrors(sFragment))
		return false;

	// link program
	GL_CHECK(m_id = glCreateProgram());
	GL_CHECK(glAttachShader(m_id, sVertex));
	GL_CHECK(glAttachShader(m_id, sFragment));
	GL_CHECK(glBindAttribLocation(m_id, 0, "position"));
	GL_CHECK(glBindAttribLocation(m_id, 1, "texCoords"));
	GL_CHECK(glLinkProgram(m_id));
	if (!CheckCompileErrors(m_id, true))
		return false;

	// delete the shaders as they're linked into our program now and no longer necessery
	GL_CHECK(glDeleteShader(sVertex));
	GL_CHECK(glDeleteShader(sFragment));
	return true;
}

bool cShader::CheckCompileErrors(GLuint object, bool program) {
	GLint success;
	GLchar infoLog[1024];
	if (!program) {
		GL_CHECK(glGetShaderiv(object, GL_COMPILE_STATUS, &success));
		if (!success) {
			GL_CHECK(glGetShaderInfoLog(object, 1024, NULL, infoLog));
			LOGERROR("openglosd: %s: Compile-time error: Type: %d - %s", __FUNCTION__, m_type, infoLog);
			return false;
		}
	} else {
		GL_CHECK(glGetProgramiv(object, GL_LINK_STATUS, &success));
		if (!success) {
			GL_CHECK(glGetProgramInfoLog(object, 1024, NULL, infoLog));
			LOGERROR("openglosd: %s: Link-time error: Type: %d - %s", __FUNCTION__, m_type, infoLog);
			return false;
		}
	}
	return true;
}

#define KERNING_UNKNOWN  (-10000)
/****************************************************************************************
* cOglGlyph
****************************************************************************************/
cOglGlyph::cOglGlyph(FT_ULong charCode, FT_BitmapGlyph ftGlyph)
	: m_charCode(charCode),
	  m_bearingLeft(ftGlyph->left),
	  m_bearingTop(ftGlyph->top),
	  m_width(ftGlyph->bitmap.width),
	  m_height(ftGlyph->bitmap.rows),
	  m_pBuffer(ftGlyph->bitmap.buffer),
	  m_advanceX(ftGlyph->root.advance.x >> 16)   // value in 1/2^16 pixel
{
}

cOglGlyph::~cOglGlyph(void)
{
	if (m_texture)
		GL_CHECK(glDeleteTextures(1, &m_texture));
}

int cOglGlyph::GetKerningCache(FT_ULong prevSym)
{
	for (int i = m_pKerningCache.Size(); --i > 0; ) {
		if (m_pKerningCache[i].prevSym == prevSym)
			return m_pKerningCache[i].kerning;
	}
	return KERNING_UNKNOWN;
}

void cOglGlyph::SetKerningCache(FT_ULong prevSym, int kerning)
{
	m_pKerningCache.Append(tKerning(prevSym, kerning));
}

void cOglGlyph::BindTexture(void)
{
	GL_CHECK(glBindTexture(GL_TEXTURE_2D, m_texture));
}

void cOglGlyph::LoadTexture(void)
{
	// Disable byte-alignment restriction
	GL_CHECK(glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
	GL_CHECK(glGenTextures(1, &m_texture));
	GL_CHECK(glBindTexture(GL_TEXTURE_2D, m_texture));

	GL_CHECK(glTexImage2D(
		GL_TEXTURE_2D,
		0,
		GL_LUMINANCE,
		m_width,
		m_height,
		0,
		GL_LUMINANCE,
		GL_UNSIGNED_BYTE,
		m_pBuffer
	));

	// Set texture options
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
	GL_CHECK(glBindTexture(GL_TEXTURE_2D, 0));
	GL_CHECK(glPixelStorei(GL_UNPACK_ALIGNMENT, 4));
}


/****************************************************************************************
* cOglAtlasGlyph
****************************************************************************************/
cOglAtlasGlyph::cOglAtlasGlyph(FT_ULong charCode, FT_BitmapGlyph ftGlyph, float offsetX, float offsetY)
	: cOglGlyph(charCode, ftGlyph),
	  m_advanceY(ftGlyph->root.advance.y >> 16),   // value in 1/2^16 pixel
	  m_offsetX(offsetX),
	  m_offsetY(offsetY)
{
}

/****************************************************************************************
* cOglFontAtlas
****************************************************************************************/
cOglFontAtlas::cOglFontAtlas(FT_Face face, int height)
{
	int maxAtlasWidth;
	GL_CHECK(glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxAtlasWidth));

	FT_Set_Pixel_Sizes(face, 0, height);
	FT_GlyphSlot g = face->glyph;

	int rowW = 0;
	int rowH = 0;

	/* Find the minimum size for the texture holding all visible ASCII characters */
	for (int i = MIN_CHARCODE; i <= MAX_CHARCODE; i++) {
		if (FT_Load_Char(face, i, FT_LOAD_NO_BITMAP)) {
			LOGDEBUG2(L_OPENGL, "openglosd: %s: Loading char %d failed!", __FUNCTION__, i);
			continue;
		}

		// do some glyph manipulation
		FT_Glyph ftGlyph;
		FT_Stroker stroker;
		if (FT_Stroker_New(g->library, &stroker)) {
			LOGERROR("openglosd: %s: FT_Stroker_New error!", __FUNCTION__);
			return;
		}

		float outlineWidth = 0.25f;
		FT_Stroker_Set(stroker, (int)(outlineWidth * 64),
					   FT_STROKER_LINECAP_ROUND, FT_STROKER_LINEJOIN_ROUND, 0);

		if (FT_Get_Glyph(g, &ftGlyph)) {
			LOGERROR("openglosd: %s: FT_Get_Glyph error!", __FUNCTION__);
			return;
		}

		if (FT_Glyph_StrokeBorder(&ftGlyph, stroker, 0, 1)) {
			LOGERROR("openglosd: %s: FT_Glyph_StrokeBoder error!", __FUNCTION__);
			return;
		}

		FT_Stroker_Done(stroker);

		if (FT_Glyph_To_Bitmap(&ftGlyph, FT_RENDER_MODE_NORMAL, 0, 1)) {
			LOGERROR("openglosd: %s: FT_Glyph_To_Bitmap error!", __FUNCTION__);
			return;
		}

		FT_BitmapGlyph bGlyph = (FT_BitmapGlyph)ftGlyph;

		if (rowW + bGlyph->bitmap.width + 1 >= (unsigned int)maxAtlasWidth) {
			m_width = std::max(m_width, rowW);
			m_height += rowH;
			rowW = 0;
			rowH = 0;
		}
		rowW += bGlyph->bitmap.width + 1;
		rowH = std::max(rowH, (int)bGlyph->bitmap.rows);

		FT_Done_Glyph(ftGlyph);
	}

	m_width = std::max(m_width, rowW);
	m_height += rowH;

	/* Create a texture that will be used to hold all ASCII glyphs */
	GL_CHECK(glGenTextures(1, &m_texture));
	GL_CHECK(glBindTexture(GL_TEXTURE_2D, m_texture));
	LOGDEBUG2(L_OPENGL, "openglosd: %s: Try creating font atlas texture with w %d h %d (max %d)", __FUNCTION__, m_width, m_height, maxAtlasWidth);

	GL_CHECK(glTexImage2D(
		GL_TEXTURE_2D,
		0,
		GL_LUMINANCE,
		m_width,
		m_height,
		0,
		GL_LUMINANCE,
		GL_UNSIGNED_BYTE,
		0
	));

	GL_CHECK(glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));

	int offsetX = 0;
	int offsetY = 0;

	rowH = 0;

	// Now do the real upload
	for (FT_ULong i = MIN_CHARCODE; i <= MAX_CHARCODE; i++) {
		if (FT_Load_Char(face, i, FT_LOAD_NO_BITMAP)) {
			LOGWARNING("openglosd: %s: Loading char %c failed!", __FUNCTION__, i);
			continue;
		}

		// do some glyph manipulation
		FT_Glyph ftGlyph;
		FT_Stroker stroker;
		if (FT_Stroker_New(g->library, &stroker)) {
			LOGERROR("openglosd: %s: FT_Stroker_New error!", __FUNCTION__);
			return;
		}

		float outlineWidth = 0.25f;
		FT_Stroker_Set(stroker, (int)(outlineWidth * 64),
					   FT_STROKER_LINECAP_ROUND, FT_STROKER_LINEJOIN_ROUND, 0);

		if (FT_Get_Glyph(g, &ftGlyph)) {
			LOGERROR("openglosd: %s: FT_Get_Glyph error!", __FUNCTION__);
			return;
		}

		if (FT_Glyph_StrokeBorder(&ftGlyph, stroker, 0, 1)) {
			LOGERROR("openglosd: %s: FT_Glyph_StrokeBoder error!", __FUNCTION__);
			return;
		}

		FT_Stroker_Done(stroker);

		if (FT_Glyph_To_Bitmap(&ftGlyph, FT_RENDER_MODE_NORMAL, 0, 1)) {
			LOGERROR("openglosd: %s: FT_Glyph_To_Bitmap error!", __FUNCTION__);
			return;
		}
		FT_BitmapGlyph bGlyph = (FT_BitmapGlyph)ftGlyph;

		// pushing the glyphs to the texture
		if (offsetX + bGlyph->bitmap.width + 1 >= (unsigned int)maxAtlasWidth) {
			offsetY += rowH;
			rowH = 0;
			offsetX = 0;
		}

		GL_CHECK(glTexSubImage2D(
			GL_TEXTURE_2D,
			0,
			offsetX,
			offsetY,
			bGlyph->bitmap.width,
			bGlyph->bitmap.rows,
			GL_LUMINANCE,
			GL_UNSIGNED_BYTE,
			bGlyph->bitmap.buffer
		));

		m_pGlyph[i - MIN_CHARCODE] = new cOglAtlasGlyph(i, bGlyph, offsetX / (float)m_width, offsetY / (float)m_height);
		rowH = std::max(rowH, (int)bGlyph->bitmap.rows);
		offsetX += bGlyph->bitmap.width + 1;

		FT_Done_Glyph(ftGlyph);
	}

	GL_CHECK(glBindTexture(GL_TEXTURE_2D, 0));
	LOGDEBUG2(L_OPENGL, "openglosd: %s: Created a %d x %d (%d kB) FontAtlas for fontsize %d, rowH %d, rowW %d",
		__FUNCTION__, m_width, m_height, m_width * m_height / 1024, height, rowH, rowW);
}

cOglFontAtlas::~cOglFontAtlas(void) {
	if (m_texture)
		GL_CHECK(glDeleteTextures(1, &m_texture));

	for (FT_ULong i = MIN_CHARCODE; i <= MAX_CHARCODE; i++) {
		if (m_pGlyph[i - MIN_CHARCODE]) {
			delete m_pGlyph[i - MIN_CHARCODE];
			m_pGlyph[i - MIN_CHARCODE] = nullptr;
		}
	}
}

cOglAtlasGlyph* cOglFontAtlas::GetGlyph(int sym) const {
	if (sym < MIN_CHARCODE || sym > MAX_CHARCODE)
		return nullptr;

	return m_pGlyph[sym - MIN_CHARCODE];
}

void cOglFontAtlas::BindTexture(void) {
	GL_CHECK(glBindTexture(GL_TEXTURE_2D, m_texture));
}

/****************************************************************************************
* cOglFont
****************************************************************************************/
FT_Library cOglFont::ftLib = 0;
cList<cOglFont> *cOglFont::fonts = 0;
bool cOglFont::initiated = false;

cOglFont::cOglFont(const char *fontName, int charHeight) : name(fontName) {
	size = charHeight;
	height = 0;
	bottom = 0;

	int error = FT_New_Face(ftLib, fontName, 0, &face);
	if (error)
		LOGERROR("openglosd: %s: failed to open %s!", __FUNCTION__, *name);

	FT_ULong charcode;
	FT_UInt gindex;
	int count = 0;
	int min_index = 0;
	int max_index = 0;

	charcode = FT_Get_First_Char(face, &gindex);
	min_index = gindex;
	max_index = gindex;
	while (gindex != 0) {
		count++;
		charcode = FT_Get_Next_Char(face, charcode, &gindex);
		min_index = std::min(min_index, (int)gindex);
		max_index = std::max(max_index, (int)gindex);
	}

	FT_Set_Char_Size(face, 0, charHeight * 64, 0, 0);
	height = (face->size->metrics.ascender - face->size->metrics.descender + 63) / 64;
	bottom = abs((face->size->metrics.descender - 63) / 64);
	this->atlas = new cOglFontAtlas(face, charHeight);
	LOGDEBUG2(L_OPENGL, "openglosd: %s: Created new font: %s (%d) height: %d, bottom: %d - %d chars (%d - %d)", __FUNCTION__, fontName, charHeight, height, bottom, count, min_index, max_index);
}

cOglFont::~cOglFont(void) {
	delete atlas;
	FT_Done_Face(face);
}

cOglFont *cOglFont::Get(const char *name, int charHeight) {
	if (!fonts)
		Init();

	cOglFont *font;
	for (font = fonts->First(); font; font = fonts->Next(font))
		if (!strcmp(font->Name(), name) && charHeight == font->Size()) {
			return font;
		}
	font = new cOglFont(name, charHeight);
	fonts->Add(font);
	return font;
}

void cOglFont::Init(void) {
	if (FT_Init_FreeType(&ftLib)) {
		LOGERROR("openglosd: %s: failed to initialize FreeType library!", __FUNCTION__);
		return;
	}
	fonts = new cList<cOglFont>;
	initiated = true;
}

void cOglFont::Cleanup(void) {
	if (!initiated)
		return;
	delete fonts;
	fonts = 0;
	if (ftLib && FT_Done_FreeType(ftLib))
		LOGERROR("openglosd: %s: failed to deinitialize FreeType library!", __FUNCTION__);

	ftLib = 0;
}

cOglGlyph* cOglFont::Glyph(FT_ULong charCode) const {
	// Non-breaking space:
	if (charCode == 0xA0)
		charCode = 0x20;

	// Lookup in cache:
	for (cOglGlyph *g = glyphCache.First(); g; g = glyphCache.Next(g)) {
		if (g->CharCode() == charCode) {
			return g;
		}
	}

	FT_UInt glyph_index = FT_Get_Char_Index(face, charCode);

	FT_Int32 loadFlags = FT_LOAD_NO_BITMAP;
	// Load glyph image into the slot (erase previous one):
	int error = FT_Load_Glyph(face, glyph_index, loadFlags);
	if (error) {
		LOGERROR("openglosd: %s: FT_Error (0x%02x) : %s", __FUNCTION__, FT_Errors[error].code, FT_Errors[error].message);
		return NULL;
	}

	FT_Glyph ftGlyph;
	FT_Stroker stroker;
	error = FT_Stroker_New( ftLib, &stroker );
	if (error) {
		LOGERROR("openglosd: %s: FT_Stroker_New FT_Error (0x%02x) : %s", __FUNCTION__, FT_Errors[error].code, FT_Errors[error].message);
		return NULL;
	}
	float outlineWidth = 0.25f;
	FT_Stroker_Set(stroker,
					(int)(outlineWidth * 64),
					FT_STROKER_LINECAP_ROUND,
					FT_STROKER_LINEJOIN_ROUND,
					0);


	error = FT_Get_Glyph(face->glyph, &ftGlyph);
	if (error) {
		LOGERROR("openglosd: %s: FT_Get_Glyph FT_Error (0x%02x) : %s", __FUNCTION__, FT_Errors[error].code, FT_Errors[error].message);
		return NULL;
	}

	error = FT_Glyph_StrokeBorder( &ftGlyph, stroker, 0, 1 );
	if ( error ) {
		LOGERROR("openglosd: %s: FT_Glyph_StrokeBorder FT_Error (0x%02x) : %s", __FUNCTION__, FT_Errors[error].code, FT_Errors[error].message);
		return NULL;
	}
	FT_Stroker_Done(stroker);

	error = FT_Glyph_To_Bitmap( &ftGlyph, FT_RENDER_MODE_NORMAL, 0, 1);
	if (error) {
		LOGERROR("openglosd: %s: FT_Glyph_To_Bitmap FT_Error (0x%02x) : %s", __FUNCTION__, FT_Errors[error].code, FT_Errors[error].message);
		return NULL;
	}

	cOglGlyph *Glyph = new cOglGlyph(charCode, (FT_BitmapGlyph)ftGlyph);
	Glyph->LoadTexture();
	glyphCache.Add(Glyph);
	FT_Done_Glyph(ftGlyph);

	return Glyph;
}

int cOglFont::Kerning(cOglGlyph *glyph, FT_ULong prevSym) const {
	int kerning = 0;
	if (glyph && prevSym) {
		kerning = glyph->GetKerningCache(prevSym);
		if (kerning == KERNING_UNKNOWN) {
			FT_Vector delta;
			FT_UInt glyph_index = FT_Get_Char_Index(face, glyph->CharCode());
			FT_UInt glyph_index_prev = FT_Get_Char_Index(face, prevSym);
			FT_Get_Kerning(face, glyph_index_prev, glyph_index, FT_KERNING_DEFAULT, &delta);
			kerning = delta.x / 64;
			glyph->SetKerningCache(prevSym, kerning);
		}
	}
	return kerning;
}

int cOglFont::AtlasKerning(cOglAtlasGlyph *glyph, FT_ULong prevSym) const {
	int kerning = 0;
	if (glyph && prevSym) {
		kerning = glyph->GetKerningCache(prevSym);
		if (kerning == KERNING_UNKNOWN) {
			FT_Vector delta;
			FT_UInt glyph_index = FT_Get_Char_Index(face, glyph->CharCode());
			FT_UInt glyph_index_prev = FT_Get_Char_Index(face, prevSym);
			FT_Get_Kerning(face, glyph_index_prev, glyph_index, FT_KERNING_DEFAULT, &delta);
			kerning = delta.x / 64;
			glyph->SetKerningCache(prevSym, kerning);
		}
	}
	return kerning;
}

/****************************************************************************************
* cOglFb
****************************************************************************************/
cOglFb::cOglFb(GLint width, GLint height, GLint viewPortWidth, GLint viewPortHeight) {
	initiated = false;
	fb = 0;
	texture = 0;
	this->width = width;
	this->height = height;
	this->viewPortWidth = viewPortWidth;
	this->viewPortHeight = viewPortHeight;
	if (width != viewPortWidth || height != viewPortHeight)
		scrollable = true;
	else
		scrollable = false;
}

cOglFb::~cOglFb(void) {
	if (texture)
		GL_CHECK(glDeleteTextures(1, &texture));
	if (fb)
		GL_CHECK(glDeleteFramebuffers(1, &fb));
}

bool cOglFb::Init(void) {
	initiated = true;
	GL_CHECK(glGenTextures(1, &texture));
	GL_CHECK(glBindTexture(GL_TEXTURE_2D, texture));
	GL_CHECK(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
	GL_CHECK(glGenFramebuffers(1, &fb));
	GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, fb));

	GL_CHECK(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0));

	GLenum fbstatus;
	GL_CHECK(fbstatus = glCheckFramebufferStatus(GL_FRAMEBUFFER));
	if(fbstatus != GL_FRAMEBUFFER_COMPLETE) {
		LOGERROR("openglosd: %s: Framebuffer is not complete!", __FUNCTION__);
		return false;
	}
	return true;
}

void cOglFb::Bind(void) {
	if (!initiated)
		Init();
	GL_CHECK(glViewport(0, 0, width, height));
	GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, fb));
}

void cOglFb::BindRead(void) {
	GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, fb));
}

void cOglFb::BindWrite(void) {
	GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, fb));
}

void cOglFb::Unbind(void) {
	GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, 0));
	GL_CHECK(glBindTexture(GL_TEXTURE_2D, 0));
}

bool cOglFb::BindTexture(void) {
	if (!initiated)
		return false;
	GL_CHECK(glBindTexture(GL_TEXTURE_2D, texture));
	return true;
}


/****************************************************************************************
* cOglOutputFb
****************************************************************************************/
cOglOutputFb::cOglOutputFb(GLint width, GLint height) : cOglFb(width, height, width, height) {
	initiated = false;
	this->width = width;
	this->height = height;
	fb = 0;
	texture = 0;
}

cOglOutputFb::~cOglOutputFb(void) {
	if (texture)
		GL_CHECK(glDeleteTextures(1, &texture));
	if (fb)
		GL_CHECK(glDeleteFramebuffers(1, &fb));
}

bool cOglOutputFb::Init(void) {
	initiated = true;
	GL_CHECK(glGenTextures(1, &texture));
	GL_CHECK(glBindTexture(GL_TEXTURE_2D, texture));
	GL_CHECK(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
	GL_CHECK(glGenFramebuffers(1, &fb));
	GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, fb));

	GL_CHECK(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0));

	GLenum fbstatus;
	GL_CHECK(fbstatus = glCheckFramebufferStatus(GL_FRAMEBUFFER));
	if(fbstatus != GL_FRAMEBUFFER_COMPLETE) {
		LOGERROR("openglosd: %s: Framebuffer is not complete (%d)!", __FUNCTION__, fbstatus);
		return false;
	}

	return true;
}

void cOglOutputFb::BindWrite(void) {
	if (!initiated)
		Init();
	GL_CHECK(glViewport(0, 0, width, height));
	GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, fb));
}

void cOglOutputFb::Unbind(void) {
	GL_CHECK(glFinish()); //??
	GL_CHECK(glBindTexture(GL_TEXTURE_2D, 0));
	GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, 0));
}

/****************************************************************************************
* cOglVb
****************************************************************************************/
static cOglVb *VertexBuffers[vbCount];

cOglVb::cOglVb(int type) {
	this->type = (eVertexBufferType)type;
	positionLoc = 0;
	texCoordsLoc = 1;
	vbo = 0;
	sizeVertex1 = 0;
	sizeVertex2 = 0;
	numVertices = 0;
	drawMode = 0;
}

cOglVb::~cOglVb(void) {
}

bool cOglVb::Init(void) {

	if (type == vbTexture) {
		//Texture VBO definition
		sizeVertex1 = 2;
		sizeVertex2 = 2;
		numVertices = 6;
		drawMode = GL_TRIANGLES;
		shader = stTexture;
	} else if (type == vbTextureSwapBR) {
		//Texture VBO definition, BR swapped
		sizeVertex1 = 2;
		sizeVertex2 = 2;
		numVertices = 6;
		drawMode = GL_TRIANGLES;
		shader = stTextureSwapBR;
	} else if (type == vbRect) {
		//Rectangle VBO definition
		sizeVertex1 = 2;
		sizeVertex2 = 0;
		numVertices = 4;
		drawMode = GL_TRIANGLE_FAN;
		shader = stRect;
	} else if (type == vbEllipse) {
		//Ellipse VBO definition
		sizeVertex1 = 2;
		sizeVertex2 = 0;
		numVertices = 182;
		drawMode = GL_TRIANGLE_FAN;
		shader = stRect;
	} else if (type == vbSlope) {
		//Slope VBO definition
		sizeVertex1 = 2;
		sizeVertex2 = 0;
		numVertices = 102;
		drawMode = GL_TRIANGLE_FAN;
		shader = stRect;
	} else if (type == vbText) {
		//Text VBO definition
		sizeVertex1 = 2;
		sizeVertex2 = 2;
		numVertices = 6;
		drawMode = GL_TRIANGLES;
		shader = stText;
	}

	GL_CHECK(glGenBuffers(1, &vbo));
	GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, vbo));

	GL_CHECK(glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * (sizeVertex1 + sizeVertex2) * numVertices, NULL, GL_DYNAMIC_DRAW));

	GL_CHECK(glEnableVertexAttribArray(positionLoc));
	GL_CHECK(glVertexAttribPointer(positionLoc, sizeVertex1, GL_FLOAT, GL_FALSE, (sizeVertex1 + sizeVertex2) * sizeof(GLfloat), (GLvoid*)0));
	if (sizeVertex2 > 0) {
		GL_CHECK(glEnableVertexAttribArray(texCoordsLoc));
		GL_CHECK(glVertexAttribPointer(texCoordsLoc, sizeVertex2, GL_FLOAT, GL_FALSE, (sizeVertex1 + sizeVertex2) * sizeof(GLfloat), (GLvoid*)(sizeVertex1 * sizeof(GLfloat))));
	}

	GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, 0));

	return true;
}

void cOglVb::Bind(void) {
	GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, vbo));
	GL_CHECK(glEnableVertexAttribArray(positionLoc));
	GL_CHECK(glVertexAttribPointer(positionLoc, sizeVertex1, GL_FLOAT, GL_FALSE, (sizeVertex1 + sizeVertex2) * sizeof(GLfloat), (GLvoid*)0));
	if (sizeVertex2 > 0) {
		GL_CHECK(glEnableVertexAttribArray(texCoordsLoc));
		GL_CHECK(glVertexAttribPointer(texCoordsLoc, sizeVertex2, GL_FLOAT, GL_FALSE, (sizeVertex1 + sizeVertex2) * sizeof(GLfloat), (GLvoid*)(sizeVertex1 * sizeof(GLfloat))));
	}
}

void cOglVb::Unbind(void) {
	GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, 0));
}

void cOglVb::ActivateShader(void) {
	Shaders[shader]->Use();
}

void cOglVb::EnableBlending(void) {
	GL_CHECK(glEnable(GL_BLEND));
	GL_CHECK(glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA));
}

void cOglVb::DisableBlending(void) {
	GL_CHECK(glDisable(GL_BLEND));
}

void cOglVb::SetShaderColor(GLint color) {
	glm::vec4 col;
	ConvertColor(color, col);
	Shaders[shader]->SetVector4f("inColor", col.r, col.g, col.b, col.a);
}

void cOglVb::SetShaderBorderColor(GLint color) {
	glm::vec4 col;
	ConvertColor(color, col);
	Shaders[shader]->SetVector4f("bColor", col.r, col.g, col.b, col.a);
}

void cOglVb::SetShaderTexture(GLint value) {
	Shaders[shader]->SetInteger("screenTexture", value);
}

void cOglVb::SetShaderAlpha(GLint alpha) {
	Shaders[shader]->SetVector4f("alpha", 1.0f, 1.0f, 1.0f, (GLfloat)(alpha) / 255.0f);
}

void cOglVb::SetShaderProjectionMatrix(GLint width, GLint height) {
	glm::mat4 projection = glm::ortho(0.0f, (GLfloat)width, (GLfloat)height, 0.0f, -1.0f, 1.0f);
	Shaders[shader]->SetMatrix4("projection", projection);
}

void cOglVb::SetVertexSubData(GLfloat *vertices, int count) {
	if (count == 0)
		count = numVertices;
	GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, vbo));
	GL_CHECK(glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(GLfloat) * (sizeVertex1 + sizeVertex2) * count, vertices));
	GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, 0));
}

void cOglVb::SetVertexData(GLfloat *vertices, int count) {
	if (count == 0)
		count = numVertices;
	GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, vbo));
	GL_CHECK(glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * (sizeVertex1 + sizeVertex2) * count, vertices, GL_DYNAMIC_DRAW));
	GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, 0));
}

void cOglVb::DrawArrays(int count) {
	if (count == 0)
		count = numVertices;
	GL_CHECK(glDrawArrays(drawMode, 0, count));
}

/****************************************************************************************
* cOpenGLCmd
****************************************************************************************/

//------------------ cOglCmdInitOutputFb --------------------
bool cOglCmdInitOutputFb::Execute(void)
{
	bool ok = m_pOutputFramebuffer->Init();
	m_pOutputFramebuffer->Unbind();
	return ok;
}

//------------------ cOglCmdInitFb --------------------------
bool cOglCmdInitFb::Execute(void)
{
	bool ok = m_pFramebuffer->Init();
	m_pFramebuffer->Unbind();
	if (m_wait)
		m_wait->Signal();
	return ok;
}

//------------------ cOglCmdDeleteFb ------------------------
bool cOglCmdDeleteFb::Execute(void)
{
	GL_CHECK(glFinish());
	if (m_pFramebuffer)
		delete m_pFramebuffer;
	return true;
}

//------------------ cOglCmdRenderFbToBufferFb --------------
bool cOglCmdRenderFbToBufferFb::Execute(void)
{
	GLfloat x1 = m_x; // left
	GLfloat y1 = m_y; // top
	GLfloat x2 = m_x + m_pFramebuffer->ViewportWidth();  // right
	GLfloat y2 = m_y + m_pFramebuffer->ViewportHeight(); // bottom

	GLfloat texX1 = m_drawPortX / (GLfloat)m_pFramebuffer->Width();
	GLfloat texX2 = texX1 + 1.0f;
	GLfloat texY1 = m_drawPortY / (GLfloat)m_pFramebuffer->Height();
	GLfloat texY2 = texY1 + 1.0f;

	if (m_pFramebuffer->Scrollable()) {
		GLfloat pageHeight = (GLfloat)m_pFramebuffer->ViewportHeight() / (GLfloat)m_pFramebuffer->Height();
		texX1 = abs(m_drawPortX) / (GLfloat)m_pFramebuffer->Width();
		texY1 = 1.0f - pageHeight - abs(m_drawPortY) / (GLfloat)m_pFramebuffer->Height();
		texX2 = texX1 + (GLfloat)m_pFramebuffer->ViewportWidth() / (GLfloat)m_pFramebuffer->Width();
		texY2 = texY1 + pageHeight;
	}

	GLfloat quadVertices[] = {
		// Pos    // TexCoords
		x1,  y1,  texX1, texY2,          // left top
		x1,  y2,  texX1, texY1,          // left bottom
		x2,  y2,  texX2, texY1,          // right bottom

		x1,  y1,  texX1, texY2,          // left top
		x2,  y2,  texX2, texY1,          // right bottom
		x2,  y1,  texX2, texY2           // right top
	};

	VertexBuffers[vbTexture]->ActivateShader();
	VertexBuffers[vbTexture]->SetShaderAlpha(m_transparency);
	VertexBuffers[vbTexture]->SetShaderProjectionMatrix(m_pBuffer->Width(), m_pBuffer->Height());
	VertexBuffers[vbTexture]->SetShaderBorderColor(m_bcolor);

	m_pBuffer->Bind();
	if (!m_pFramebuffer->BindTexture())
		return false;
	if (!m_alphablending)
		VertexBuffers[vbTexture]->DisableBlending();
	VertexBuffers[vbTexture]->Bind();
	GL_CHECK(glEnable(GL_SCISSOR_TEST));
	GL_CHECK(glScissor(m_dirtyX, m_pBuffer->Height() - m_dirtyTop - m_dirtyHeight, m_dirtyWidth, m_dirtyHeight));
	VertexBuffers[vbTexture]->SetVertexSubData(quadVertices);
	VertexBuffers[vbTexture]->DrawArrays();
	GL_CHECK(glDisable(GL_SCISSOR_TEST));
	VertexBuffers[vbTexture]->Unbind();

#ifdef WRITE_PNG
	// Read back bFb framebuffer
//	if (Device->WritePngs())
//		writePng(0, 0, buffer->Width(), buffer->Height(), false);
#endif
	if (!m_alphablending)
		VertexBuffers[vbTexture]->EnableBlending();
	m_pBuffer->Unbind();

	return true;
}

//------------------ cOglCmdCopyBufferToOutputFb --------------------
bool cOglCmdCopyBufferToOutputFb::Execute(void)
{
	GLfloat x1 = m_x;
	GLfloat y1 = m_y;
	GLfloat x2 = m_x + (GLfloat)m_pFramebuffer->Width();
	GLfloat y2 = m_y + (GLfloat)m_pFramebuffer->Height();

	GLfloat texX1 = 0.0f;
	GLfloat texX2 = 1.0f;
	GLfloat texY1 = 1.0f;
	GLfloat texY2 = 0.0f;

	GLfloat quadVertices[] = {
		// Pos    // TexCoords
		x1,  y1,  texX1, texY1,          //left top
		x1,  y2,  texX1, texY2,          //left bottom
		x2,  y2,  texX2, texY2,          //right bottom

		x1,  y1,  texX1, texY1,          //left top
		x2,  y2,  texX2, texY2,          //right bottom
		x2,  y1,  texX2, texY1           //right top
	};

	VertexBuffers[vbTexture]->ActivateShader();
	VertexBuffers[vbTexture]->SetShaderAlpha(255);
	VertexBuffers[vbTexture]->SetShaderProjectionMatrix(m_pOutputFramebuffer->Width(), m_pOutputFramebuffer->Height());
	VertexBuffers[vbTexture]->SetShaderBorderColor(m_borderColor);

	m_pOutputFramebuffer->Bind();
	GL_CHECK(glViewport(0, 0, m_pOutputFramebuffer->Width(), m_pOutputFramebuffer->Height()));
	if (!m_pFramebuffer->BindTexture())
		return false;

	VertexBuffers[vbTexture]->Bind();
	VertexBuffers[vbTexture]->SetVertexSubData(quadVertices);
	VertexBuffers[vbTexture]->DrawArrays();
	VertexBuffers[vbTexture]->Unbind();

	GL_CHECK(glFinish());
	// eglSwapBuffers and gbm_surface_lock_front_buffer in OsdDrawARGB()
	if (m_active)
		m_pDevice->OsdDrawARGB(0, 0, m_pOutputFramebuffer->Width(), m_pOutputFramebuffer->Height(), 0, 0, 0, 0);
	else
		m_pDevice->OsdClose();

#ifdef WRITE_PNG
	// Read back oFb framebuffer
	if (m_pDevice->WritePngs())
		writePng(0, 0, m_pOutputFramebuffer->Width(), m_pOutputFramebuffer->Height(), true);
#endif
	m_pOutputFramebuffer->Unbind();

	return true;
}

//------------------ cOglCmdFill --------------------
bool cOglCmdFill::Execute(void)
{
	glm::vec4 col;
	ConvertColor(m_color, col);
	m_pFramebuffer->Bind();
	GL_CHECK(glClearColor(col.r, col.g, col.b, col.a));
	GL_CHECK(glClear(GL_COLOR_BUFFER_BIT));
	m_pFramebuffer->Unbind();

	return true;
}

//------------------ cOglCmdBufferFill --------------------
bool cOglCmdBufferFill::Execute(void)
{
	glm::vec4 col;
	ConvertColor(m_color, col);
	GL_CHECK(glClearColor(col.r, col.g, col.b, col.a));
	GL_CHECK(glClear(GL_COLOR_BUFFER_BIT));

	return true;
}

//------------------ cOglCmdDrawRectangle --------------------
bool cOglCmdDrawRectangle::Execute(void)
{
	if (m_width <= 0 || m_height <= 0)
		return false;

	GLfloat x1 = m_x;
	GLfloat y1 = m_y;
	GLfloat x2 = m_x + m_width;
	GLfloat y2 = m_y + m_height;

	GLfloat vertices[] = {
		x1, y1,    // left top
		x2, y1,    // right top
		x2, y2,    // right bottom
		x1, y2     // left bottom
	};

	VertexBuffers[vbRect]->ActivateShader();
	VertexBuffers[vbRect]->SetShaderColor(m_color);
	VertexBuffers[vbRect]->SetShaderProjectionMatrix(m_pFramebuffer->Width(), m_pFramebuffer->Height());

	m_pFramebuffer->Bind();
	VertexBuffers[vbRect]->DisableBlending();
	VertexBuffers[vbRect]->Bind();
	VertexBuffers[vbRect]->SetVertexSubData(vertices);
	VertexBuffers[vbRect]->DrawArrays();
	VertexBuffers[vbRect]->Unbind();
	VertexBuffers[vbRect]->EnableBlending();
	m_pFramebuffer->Unbind();

	return true;
}

//------------------ cOglCmdDrawEllipse --------------------
// quadrants:
// 0       draws the entire ellipse
// 1..4    draws only the first, second, third or fourth quadrant, respectively
// 5..8    draws the right, top, left or bottom half, respectively
// -1..-4  draws the inverted part of the given quadrant
bool cOglCmdDrawEllipse::Execute(void)
{
	if (m_width <= 0 || m_height <= 0)
		return false;

	int numVertices = 0;
	GLfloat *vertices = NULL;

	switch (m_quadrants) {
		case 0:
			vertices = CreateVerticesFull(numVertices);
			break;
		case 1:
		case 2:
		case 3:
		case 4:
		case -1:
		case -2:
		case -3:
		case -4:
			vertices = CreateVerticesQuadrant(numVertices);
			break;
		case 5:
		case 6:
		case 7:
		case 8:
			vertices = CreateVerticesHalf(numVertices);
			break;
		default:
			break;
	}

	VertexBuffers[vbEllipse]->ActivateShader();
	VertexBuffers[vbEllipse]->SetShaderColor(m_color);
	VertexBuffers[vbEllipse]->SetShaderProjectionMatrix(m_pFramebuffer->Width(), m_pFramebuffer->Height());

	// not antialiased
	m_pFramebuffer->Bind();
	VertexBuffers[vbEllipse]->DisableBlending();
	VertexBuffers[vbEllipse]->Bind();
	VertexBuffers[vbEllipse]->SetVertexSubData(vertices, numVertices);
	VertexBuffers[vbEllipse]->DrawArrays(numVertices);
	VertexBuffers[vbEllipse]->Unbind();
	VertexBuffers[vbEllipse]->EnableBlending();
	m_pFramebuffer->Unbind();

	delete[] vertices;
	return true;
}

GLfloat *cOglCmdDrawEllipse::CreateVerticesFull(int &numVertices)
{
	int size = 364;
	numVertices = size/2;
	GLfloat radiusX = (GLfloat)m_width / 2;
	GLfloat radiusY = (GLfloat)m_height / 2;
	GLfloat *vertices = new GLfloat[size];
	vertices[0] = m_x + radiusX;
	vertices[1] = m_y + radiusY;
	for (int i=0; i <= 180; i++) {
		vertices[2 * i + 2] = m_x + radiusX + (GLfloat)cos(2 * i * M_PI / 180.0f) * radiusX;
		vertices[2 * i + 3] = m_y + radiusY - (GLfloat)sin(2 * i * M_PI / 180.0f) * radiusY;
	}
	return vertices;
}

GLfloat *cOglCmdDrawEllipse::CreateVerticesQuadrant(int &numVertices)
{
	int size = 94;
	numVertices = size / 2;
	GLfloat radiusX = (GLfloat)m_width;
	GLfloat radiusY = (GLfloat)m_height;
	GLint transX = 0;
	GLint transY = 0;
	GLint startAngle = 0;
	GLfloat *vertices = new GLfloat[size];
	switch (m_quadrants) {
		case 1:
			vertices[0] = m_x;
			vertices[1] = m_y + m_height;
			transY = radiusY;
			break;
		case 2:
			vertices[0] = m_x + m_width;
			vertices[1] = m_y + m_height;
			transX = radiusX;
			transY = radiusY;
			startAngle = 90;
			break;
		case 3:
			vertices[0] = m_x + m_width;
			vertices[1] = m_y;
			transX = radiusX;
			startAngle = 180;
			break;
		case 4:
			vertices[0] = m_x;
			vertices[1] = m_y;
			startAngle = 270;
			break;
		case -1:
			vertices[0] = m_x + m_width;
			vertices[1] = m_y;
			transY = radiusY;
			break;
		case -2:
			vertices[0] = m_x;
			vertices[1] = m_y;
			transX = radiusX;
			transY = radiusY;
			startAngle = 90;
			break;
		case -3:
			vertices[0] = m_x;
			vertices[1] = m_y + m_height;
			transX = radiusX;
			startAngle = 180;
			break;
		case -4:
			vertices[0] = m_x + m_width;
			vertices[1] = m_y + m_height;
			startAngle = 270;
			break;
		default:
			break;
	}
	for (int i = 0; i <= 45; i++) {
		vertices[2 * i + 2] = m_x + transX + (GLfloat)cos((2 * i + startAngle) * M_PI / 180.0f) * radiusX;
		vertices[2 * i + 3] = m_y + transY - (GLfloat)sin((2 * i + startAngle) * M_PI / 180.0f) * radiusY;
	}
	return vertices;
}

GLfloat *cOglCmdDrawEllipse::CreateVerticesHalf(int &numVertices)
{
	int size = 184;
	numVertices = size / 2;
	GLfloat radiusX = 0.0f;
	GLfloat radiusY = 0.0f;
	GLint transX = 0;
	GLint transY = 0;
	GLint startAngle = 0;
	GLfloat *vertices = new GLfloat[size];
	switch (m_quadrants) {
		case 5:
			radiusX = (GLfloat)m_width;
			radiusY = (GLfloat)m_height / 2;
			vertices[0] = m_x;
			vertices[1] = m_y + radiusY;
			startAngle = 270;
			transY = radiusY;
			break;
		case 6:
			radiusX = (GLfloat)m_width / 2;
			radiusY = (GLfloat)m_height;
			vertices[0] = m_x + radiusX;
			vertices[1] = m_y + radiusY;
			startAngle = 0;
			transX = radiusX;
			transY = radiusY;
			break;
		case 7:
			radiusX = (GLfloat)m_width;
			radiusY = (GLfloat)m_height / 2;
			vertices[0] = m_x + radiusX;
			vertices[1] = m_y + radiusY;
			startAngle = 90;
			transX = radiusX;
			transY = radiusY;
			break;
		case 8:
			radiusX = (GLfloat)m_width / 2;
			radiusY = (GLfloat)m_height;
			vertices[0] = m_x + radiusX;
			vertices[1] = m_y;
			startAngle = 180;
			transX = radiusX;
			break;
		default:
			break;
	}
	for (int i=0; i <= 90; i++) {
		vertices[2 * i + 2] = m_x + transX + (GLfloat)cos((2 * i + startAngle) * M_PI / 180.0f) * radiusX;
		vertices[2 * i + 3] = m_y + transY - (GLfloat)sin((2 * i + startAngle) * M_PI / 180.0f) * radiusY;
	}
	return vertices;
}

//------------------ cOglCmdDrawSlope --------------------
// type:
// 0: horizontal, rising,  lower
// 1: horizontal, rising,  upper
// 2: horizontal, falling, lower
// 3: horizontal, falling, upper
// 4: vertical,   rising,  lower
// 5: vertical,   rising,  upper
// 6: vertical,   falling, lower
// 7: vertical,   falling, upper
bool cOglCmdDrawSlope::Execute(void)
{
	if (m_width <= 0 || m_height <= 0)
		return false;

	bool falling  = m_type & 0x02;
	bool vertical = m_type & 0x04;

	int steps = 100;
	if (m_width < 100)
		steps = 25;
	int numVertices = steps + 2;
	GLfloat *vertices = new GLfloat[numVertices * 2];

	switch (m_type) {
		case 0:
		case 4:
			vertices[0] = (GLfloat)(m_x + m_width);
			vertices[1] = (GLfloat)(m_y + m_height);
			break;
		case 1:
		case 5:
			vertices[0] = (GLfloat)m_x;
			vertices[1] = (GLfloat)m_y;
			break;
		case 2:
		case 6:
			vertices[0] = (GLfloat)m_x;
			vertices[1] = (GLfloat)(m_y + m_height);
			break;
		case 3:
		case 7:
			vertices[0] = (GLfloat)(m_x + m_width);
			vertices[1] = (GLfloat)m_y;
			break;
		default:
			vertices[0] = (GLfloat)(m_x);
			vertices[1] = (GLfloat)(m_y);
			break;
	}

	for (int i = 0; i <= steps; i++) {
		GLfloat c = cos(i * M_PI / steps);
		if (falling)
			c = -c;
		if (vertical) {
			vertices[2 * i + 2] = (GLfloat)m_x + (GLfloat)m_width / 2.0f + (GLfloat)m_width * c / 2.0f;
			vertices[2 * i + 3] = (GLfloat)m_y + (GLfloat)i * ((GLfloat)m_height) / steps ;
		} else {
			vertices[2 * i + 2] = (GLfloat)m_x + (GLfloat)i * ((GLfloat)m_width) / steps ;
			vertices[2 * i + 3] = (GLfloat)m_y + (GLfloat)m_height / 2.0f + (GLfloat)m_height * c / 2.0f;
		}
	}

	VertexBuffers[vbSlope]->ActivateShader();
	VertexBuffers[vbSlope]->SetShaderColor(m_color);
	VertexBuffers[vbSlope]->SetShaderProjectionMatrix(m_pFramebuffer->Width(), m_pFramebuffer->Height());

	// not antialiased
	m_pFramebuffer->Bind();
	VertexBuffers[vbSlope]->DisableBlending();
	VertexBuffers[vbSlope]->Bind();
	VertexBuffers[vbSlope]->SetVertexSubData(vertices, numVertices);
	VertexBuffers[vbSlope]->DrawArrays(numVertices);
	VertexBuffers[vbSlope]->Unbind();
	VertexBuffers[vbSlope]->EnableBlending();
	m_pFramebuffer->Unbind();

	delete[] vertices;
	return true;
}

//------------------ cOglCmdDrawText --------------------
bool cOglCmdDrawText::Execute(void)
{
	cOglFont *f = cOglFont::Get(*m_fontName, m_fontSize);
	if (!f)
		return false;

	if (!m_length)
		return false;

	VertexBuffers[vbText]->ActivateShader();
	VertexBuffers[vbText]->SetShaderColor(m_colorText);
	VertexBuffers[vbText]->SetShaderProjectionMatrix(m_pFramebuffer->Width(), m_pFramebuffer->Height());

	m_pFramebuffer->Bind();
	VertexBuffers[vbText]->Bind();

	int xGlyph = m_x;
	int yGlyph = m_y;
	int fontHeight = f->Height();
	int bottom = f->Bottom();
	FT_ULong sym = 0;
	FT_ULong prevSym = 0;
	int kerning = 0;

	// Check, if we only have symbols, which are in our atlas
	int unknown_char = 0;
	for (int i = 0; m_pSymbols[i]; i++) {
		if ((m_pSymbols[i] < MIN_CHARCODE) || (m_pSymbols[i] > MAX_CHARCODE)) {
			if (m_pSymbols[i]) {
				unknown_char = m_pSymbols[i];
				break;
			}
		}
	}

	if (!unknown_char) {
		cOglFontAtlas *fa = f->Atlas();
		std::vector<GLfloat> vertices;
		vertices.reserve( 4 * 6 * m_length);

		for (int i = 0; m_pSymbols[i]; i++) {
			sym = m_pSymbols[i];

			cOglAtlasGlyph *g;
			// Get the glyph from the font atlas for ASCII code MIN_CHARCODE-MAX_CHARCODE
			g = fa->GetGlyph(sym);

			if (!g) {
				LOGWARNING("openglosd: %s: could not load glyph %lx", __FUNCTION__, sym);
				continue;
			}

			if ( m_limitX && xGlyph + g->AdvanceX() > m_limitX )
				break;

			kerning = f->AtlasKerning(g, prevSym);
			prevSym = sym;

			GLfloat x2 = xGlyph + kerning + g->BearingLeft();
			GLfloat y2 = m_y + (fontHeight - bottom - g->BearingTop());  //top
			GLfloat w = g->Width();
			GLfloat h = g->Height();

			vertices.push_back(x2);
			vertices.push_back(y2);
			vertices.push_back(g->OffsetX());
			vertices.push_back(g->OffsetY());

			vertices.push_back(x2 + w);
			vertices.push_back(y2);
			vertices.push_back(g->OffsetX() + g->Width() / (float)fa->Width());
			vertices.push_back(g->OffsetY());

			vertices.push_back(x2);
			vertices.push_back(y2 + h);
			vertices.push_back(g->OffsetX());
			vertices.push_back(g->OffsetY() + g->Height() / (float)fa->Height());

			vertices.push_back(x2 + w);
			vertices.push_back(y2);
			vertices.push_back(g->OffsetX() + g->Width() / (float)fa->Width());
			vertices.push_back(g->OffsetY());

			vertices.push_back(x2);
			vertices.push_back(y2 + h);
			vertices.push_back(g->OffsetX());
			vertices.push_back(g->OffsetY() + g->Height() / (float)fa->Height());

			vertices.push_back(x2 + w);
			vertices.push_back(y2 + h);
			vertices.push_back(g->OffsetX() + g->Width() / (float)fa->Width());
			vertices.push_back(g->OffsetY() + g->Height() / (float)fa->Height());

			xGlyph += kerning + g->AdvanceX();
			yGlyph += kerning + g->AdvanceY();


			if ( xGlyph > m_pFramebuffer->Width() - 1 )
				break;
		}

		fa->BindTexture();
		VertexBuffers[vbText]->SetVertexData(vertices.data(), (vertices.size() / 4));
		VertexBuffers[vbText]->DrawArrays(vertices.size() / 4);
	} else {
		LOGDEBUG2(L_OPENGL, "openglosd: %s: char %d is not on the texture atlas, use single draw", __FUNCTION__, unknown_char);
		for (int i = 0; m_pSymbols[i]; i++) {
			sym = m_pSymbols[i];
			cOglGlyph *g = f->Glyph(sym);
			if (!g) {
				LOGWARNING("openglosd: %s: could not load glyph %lx", __FUNCTION__, sym);
				continue;
			}

			if ( m_limitX && xGlyph + g->AdvanceX() > m_limitX )
				break;

			kerning = f->Kerning(g, prevSym);
			prevSym = sym;

			GLfloat x1 = xGlyph + kerning + g->BearingLeft();            //left
			GLfloat y1 = m_y + (fontHeight - bottom - g->BearingTop());  //top
			GLfloat x2 = x1 + g->Width();                                //right
			GLfloat y2 = y1 + g->Height();                               //bottom

			GLfloat vertices[] = {
				x1, y2,   0.0, 1.0,     // left bottom
				x1, y1,   0.0, 0.0,     // left top
				x2, y1,   1.0, 0.0,     // right top

				x1, y2,   0.0, 1.0,     // left bottom
				x2, y1,   1.0, 0.0,     // right top
				x2, y2,   1.0, 1.0      // right bottom
			};

			g->BindTexture();
			VertexBuffers[vbText]->SetVertexData(vertices);
			VertexBuffers[vbText]->DrawArrays();

			xGlyph += kerning + g->AdvanceX();

			if ( xGlyph > m_pFramebuffer->Width() - 1 )
				break;
		}
	}

	GL_CHECK(glBindTexture(GL_TEXTURE_2D, 0));
	VertexBuffers[vbText]->Unbind();
	m_pFramebuffer->Unbind();
	return true;
}

//------------------ cOglCmdDrawImage --------------------
bool cOglCmdDrawImage::Execute(void)
{
	if (m_width <= 0 || m_height <= 0)
		return false;

	GLuint texture;
	GL_CHECK(glGenTextures(1, &texture));
	GL_CHECK(glBindTexture(GL_TEXTURE_2D, texture));
	GL_CHECK(glTexImage2D(
		GL_TEXTURE_2D,
		0,
		GL_RGBA,
		m_width,
		m_height,
		0,
		GL_RGBA,
		GL_UNSIGNED_BYTE,
		m_argb
	));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
	GL_CHECK(glBindTexture(GL_TEXTURE_2D, 0));

	GLfloat x1 = m_x;                       //left
	GLfloat y1 = m_y;                       //top
	GLfloat x2 = m_x + m_width * m_scaleX;  //right
	GLfloat y2 = m_y + m_height * m_scaleY; //bottom

	GLfloat quadVertices[] = {
		x1, y2,   0.0, 1.0,     // left bottom
		x1, y1,   0.0, 0.0,     // left top
		x2, y1,   1.0, 0.0,     // right top

		x1, y2,   0.0, 1.0,     // left bottom
		x2, y1,   1.0, 0.0,     // right top
		x2, y2,   1.0, 1.0      // right bottom
	};

	VertexBuffers[vbTextureSwapBR]->ActivateShader();
	VertexBuffers[vbTextureSwapBR]->SetShaderAlpha(255);
	VertexBuffers[vbTextureSwapBR]->SetShaderProjectionMatrix(m_pFramebuffer->Width(), m_pFramebuffer->Height());
	VertexBuffers[vbTextureSwapBR]->SetShaderBorderColor(m_borderColor);

	m_pFramebuffer->Bind();
	GL_CHECK(glBindTexture(GL_TEXTURE_2D, texture));
	if (m_overlay)
		VertexBuffers[vbTextureSwapBR]->DisableBlending();
	VertexBuffers[vbTextureSwapBR]->Bind();
	VertexBuffers[vbTextureSwapBR]->SetVertexSubData(quadVertices);
	VertexBuffers[vbTextureSwapBR]->DrawArrays();
	VertexBuffers[vbTextureSwapBR]->Unbind();
	if (m_overlay)
		VertexBuffers[vbTextureSwapBR]->EnableBlending();
	m_pFramebuffer->Unbind();
	GL_CHECK(glBindTexture(GL_TEXTURE_2D, 0));
	GL_CHECK(glDeleteTextures(1, &texture));

	return true;
}

//------------------ cOglCmdDrawTexture --------------------
bool cOglCmdDrawTexture::Execute(void)
{
	if (m_pImageRef->width <= 0 || m_pImageRef->height <= 0)
		return false;

	GLfloat x1 = m_x;                                  // top
	GLfloat y1 = m_y;                                  // left
	GLfloat x2 = m_x + m_pImageRef->width * m_scaleX;  // right
	GLfloat y2 = m_y + m_pImageRef->height * m_scaleY; // bottom

	GLfloat quadVertices[] = {
		// Pos    // TexCoords
		x1,  y1,  0.0f, 0.0f,          // left bottom
		x1,  y2,  0.0f, 1.0f,          // left top
		x2,  y2,  1.0f, 1.0f,          // right top

		x1,  y1,  0.0f, 0.0f,          // left bottom
		x2,  y2,  1.0f, 1.0f,          // right top
		x2,  y1,  1.0f, 0.0f           // right bottom
	};

	VertexBuffers[vbTextureSwapBR]->ActivateShader();
	VertexBuffers[vbTextureSwapBR]->SetShaderAlpha(255);
	VertexBuffers[vbTextureSwapBR]->SetShaderProjectionMatrix(m_pFramebuffer->Width(), m_pFramebuffer->Height());
	VertexBuffers[vbTextureSwapBR]->SetShaderBorderColor(m_borderColor);

	m_pFramebuffer->Bind();
	GL_CHECK(glBindTexture(GL_TEXTURE_2D, m_pImageRef->texture));
	VertexBuffers[vbTextureSwapBR]->Bind();
	VertexBuffers[vbTextureSwapBR]->SetVertexSubData(quadVertices);
	VertexBuffers[vbTextureSwapBR]->DrawArrays();
	VertexBuffers[vbTextureSwapBR]->Unbind();
	m_pFramebuffer->Unbind();

	return true;
}


//------------------ cOglCmdStoreImage --------------------
bool cOglCmdStoreImage::Execute(void) {
	GL_CHECK(glGenTextures(1, &m_pImageRef->texture));
	GL_CHECK(glBindTexture(GL_TEXTURE_2D, m_pImageRef->texture));
	GL_CHECK(glTexImage2D(
		GL_TEXTURE_2D,
		0,
		GL_RGBA,
		m_pImageRef->width,
		m_pImageRef->height,
		0,
		GL_RGBA,
		GL_UNSIGNED_BYTE,
		m_pData
	));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
	GL_CHECK(glBindTexture(GL_TEXTURE_2D, 0));
	return true;
}

//------------------ cOglCmdDropImage --------------------
bool cOglCmdDropImage::Execute(void) {
	if (m_pImageRef->texture != GL_NONE)
		GL_CHECK(glDeleteTextures(1, &m_pImageRef->texture));
	m_pWait->Signal();
	return true;
}

/******************************************************************************
* cOglThread
******************************************************************************/
cOglThread::cOglThread(cCondWait *startWait, int maxCacheSize, cSoftHdDevice *device) : cThread("oglThread") {
	stalled = false;
	memCached = 0;
	this->maxCacheSize = maxCacheSize * 1024 * 1024;
	this->startWait = startWait;
	this->Render = device->Render();
	wait = new cCondWait();
	maxTextureSize = 0;
	for (int i = 0; i < OGL_MAX_OSDIMAGES; i++) {
		imageCache[i].used = false;
		imageCache[i].texture = GL_NONE;
		imageCache[i].width = 0;
		imageCache[i].height = 0;
	}

	Start();
}

cOglThread::~cOglThread() {
	delete wait;
	wait = NULL;
}

void cOglThread::Stop(void) {
	for (int i = 0; i < OGL_MAX_OSDIMAGES; i++) {
		if (imageCache[i].used) {
			DropImageData(i);
		}
	}
	Cancel(2);
	stalled = false;
}

void cOglThread::DoCmd(cOglCmd* cmd) {
	while (stalled)
		cCondWait::SleepMs(10);

	bool doSignal = false;
	Lock();
	if (commands.size() == 0)
		doSignal = true;
	commands.push(cmd);
	Unlock();

	if (commands.size() > OGL_CMDQUEUE_SIZE) {
		stalled = true;
	}

	if (doSignal || stalled)
		wait->Signal();
}

int cOglThread::StoreImage(const cImage &image) {
	if (!maxCacheSize) {
		LOGERROR("openglosd: %s: cannot store image, no cache set", __FUNCTION__);
		return 0;
	}

	if (image.Width() > maxTextureSize || image.Height() > maxTextureSize) {
		LOGERROR("openglosd: %s: cannot store image of %dpx x %dpx "
				"(maximum size is %dpx x %dpx) - falling back to "
				"cOsdProvider::StoreImageData()", __FUNCTION__,
				image.Width(), image.Height(),
				maxTextureSize, maxTextureSize);
		return 0;
	}

	int imgSize = image.Width() * image.Height();
	int newMemUsed = imgSize * sizeof(tColor) + memCached;
	if (newMemUsed > maxCacheSize) {
		float cachedMB = memCached / 1024.0f / 1024.0f;
		float maxMB = maxCacheSize / 1024.0f / 1024.0f;
		LOGERROR("openglosd: %s: Maximum size for GPU cache reached. Used: %.2fMB Max: %.2fMB", __FUNCTION__, cachedMB, maxMB);
		return 0;
	}

	int slot = GetFreeSlot();
	if (!slot)
		return 0;

	tColor *argb = MALLOC(tColor, imgSize);
	if (!argb) {
		LOGERROR("openglosd: %s: memory allocation of %d kb for OSD image failed", __FUNCTION__, (int)(imgSize  * sizeof(tColor) / 1024));
		ClearSlot(slot);
		slot = 0;
		return 0;
	}

	memcpy(argb, image.Data(), sizeof(tColor) * imgSize);

	sOglImage *imageRef = GetImageRef(slot);
	imageRef->width = image.Width();
	imageRef->height = image.Height();
	DoCmd(new cOglCmdStoreImage(imageRef, argb));

	cTimeMs timer(5000);
	while (imageRef->used && imageRef->texture == 0 && !timer.TimedOut())
		cCondWait::SleepMs(2);

	if (imageRef->texture == GL_NONE) {
		LOGERROR("openglosd: %s: failed to store OSD image texture! (%s)", __FUNCTION__, timer.TimedOut() ? "timed out" : "allocation failed");
		DropImageData(slot);
		slot = 0;
	}

	memCached += imgSize  * sizeof(tColor);
	return slot;
}

int cOglThread::GetFreeSlot(void) {
	Lock();
	int slot = 0;
	for (int i = 0; i < OGL_MAX_OSDIMAGES && !slot; i++) {
		if (!imageCache[i].used) {
			imageCache[i].used = true;
			slot = -i - 1;
		}
	}
	Unlock();
	return slot;
}

void cOglThread::ClearSlot(int slot) {
	int i = -slot - 1;
	if (i >= 0 && i < OGL_MAX_OSDIMAGES) {
		Lock();
		imageCache[i].used = false;
		imageCache[i].texture = GL_NONE;
		imageCache[i].width = 0;
		imageCache[i].height = 0;
		Unlock();
	}
}

sOglImage *cOglThread::GetImageRef(int slot) {
	int i = -slot - 1;
	if (0 <= i && i < OGL_MAX_OSDIMAGES)
		return &imageCache[i];
	return 0;
}

void cOglThread::DropImageData(int imageHandle) {
	sOglImage *imageRef = GetImageRef(imageHandle);
	if (!imageRef)
		return;
	int imgSize = imageRef->width * imageRef->height * sizeof(tColor);
	memCached -= imgSize;
	cCondWait dropWait;
	DoCmd(new cOglCmdDropImage(imageRef, &dropWait));
	dropWait.Wait();
	ClearSlot(imageHandle);
}

void cOglThread::Action(void) {
	if (!InitOpenGL()) {
		LOGERROR("openglosd: %s: Could not initiate OpenGL context", __FUNCTION__);
		Cleanup();
		startWait->Signal();
		return;
	}

	if (!InitShaders()) {
		LOGERROR("openglosd: %s: Could not initiate shaders", __FUNCTION__);
		Cleanup();
		startWait->Signal();
		return;
	}

	if (!InitVertexBuffers()) {
		LOGERROR("openglosd: %s: Vertex Buffers NOT initialized", __FUNCTION__);
		Cleanup();
		startWait->Signal();
		return;
	}

	GL_CHECK(glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize));
	LOGDEBUG2(L_OPENGL, "openglosd: %s: Maximum Pixmap size: %dx%dpx", __FUNCTION__, maxTextureSize, maxTextureSize);

	//now Thread is ready to do his job
	startWait->Signal();
	stalled = false;

	LOGINFO("OpenGL context initialized");

	uint64_t start_flush = 0;
	uint64_t end_flush = 0;
	int time_reset = 0;

	while(Running()) {

		if (commands.empty()) {
			wait->Wait(20);
			continue;
		}

		Lock();
		cOglCmd* cmd = commands.front();
		commands.pop();
		Unlock();

		uint64_t start = cTimeMs::Now();
		if (strcmp(cmd->Description(), "InitFramebuffer") == 0 || time_reset) {
			start_flush = cTimeMs::Now();
			time_reset = 0;
		}

		cmd->Execute();
		LOGDEBUG2(L_OPENGL_TIME_ALL, "openglosd: %s: \"%-*s\", %dms, %d commands left, time %" PRIu64 "", __FUNCTION__, 15, cmd->Description(), (int)(cTimeMs::Now() - start), (int)(commands.size()), cTimeMs::Now());

		if (strcmp(cmd->Description(), "Copy buffer to OutputFramebuffer") == 0) {
			end_flush = cTimeMs::Now();
			time_reset = 1;
			LOGDEBUG2(L_OPENGL_TIME, "openglosd: %s: OSD Flush %dms, time %" PRIu64 "", __FUNCTION__, (int)(end_flush - start_flush), cTimeMs::Now());
		}
		delete cmd;
		if (stalled && commands.size() < OGL_CMDQUEUE_SIZE / 2)
			stalled = false;
	}

	LOGDEBUG2(L_OPENGL, "openglosd: %s: Cleaning up OpenGL stuff", __FUNCTION__);
	Cleanup();
	LOGDEBUG2(L_OPENGL, "openglosd: %s: OpenGL worker thread ended", __FUNCTION__);
}

void cOglThread::eglAcquireContext(void)
{
	EGL_CHECK(eglMakeCurrent(Render->EglDisplay(), Render->EglSurface(), Render->EglSurface(), Render->EglContext()));
}

void cOglThread::eglReleaseContext(void)
{
	EGL_CHECK(eglMakeCurrent(Render->EglDisplay(), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
}

bool cOglThread::InitOpenGL(void) {
	LOGDEBUG2(L_OPENGL, "openglosd: %s: Init OpenGL context", __FUNCTION__);

	// Wait for the EGL context to be created
	while(!Render->GlInitiated()) {
		LOGDEBUG2(L_OPENGL, "openglosd: %s: wait for EGL context", __FUNCTION__);
		usleep(20000);
	}

	eglAcquireContext(); /* eglMakeCurrent with new eglSurface */

	GL_CHECK(LOGDEBUG2(L_OPENGL, "  GL Version: \"%s\"", glGetString(GL_VERSION)));
	GL_CHECK(LOGDEBUG2(L_OPENGL, "  GL Vendor: \"%s\"", glGetString(GL_VENDOR)));
	GL_CHECK(LOGDEBUG2(L_OPENGL, "  GL Extensions: \"%s\"", glGetString(GL_EXTENSIONS)));
	GL_CHECK(LOGDEBUG2(L_OPENGL, "  GL Renderer: \"%s\"", glGetString(GL_RENDERER)));

	VertexBuffers[vbText]->EnableBlending();
	GL_CHECK(glDisable(GL_DEPTH_TEST));
	LOGDEBUG2(L_OPENGL, "openglosd: %s: Init OpenGL context done", __FUNCTION__);
	return true;
}

bool cOglThread::InitShaders(void) {
	for (int i=0; i < stCount; i++) {
		cShader *shader = new cShader();
		if (!shader->Load((eShaderType)i))
			return false;
		Shaders[i] = shader;
	}
	LOGDEBUG2(L_OPENGL, "openglosd: %s: Shaders initialized", __FUNCTION__);
	return true;
}

void cOglThread::DeleteShaders(void) {
	for (int i=0; i < stCount; i++)
		delete Shaders[i];
}

bool cOglThread::InitVertexBuffers(void) {
	for (int i=0; i < vbCount; i++) {
		cOglVb *vb = new cOglVb(i);
		if (!vb->Init())
			return false;
		VertexBuffers[i] = vb;
	}
	LOGDEBUG2(L_OPENGL, "openglosd: %s: Vertex buffers initialized", __FUNCTION__);
	return true;
}

void cOglThread::DeleteVertexBuffers(void) {
	for (int i=0; i < vbCount; i++) {
		delete VertexBuffers[i];
	}
}

void cOglThread::Cleanup(void) {
	DeleteVertexBuffers();
	delete cOglOsd::OutputFramebuffer;
	cOglOsd::OutputFramebuffer = NULL;
	DeleteShaders();
	cOglFont::Cleanup();
}

/****************************************************************************************
* cOglPixmap
****************************************************************************************/

cOglPixmap::cOglPixmap(std::shared_ptr<cOglThread> oglThread, int layer, const cRect &viewPort, const cRect &drawPort)
	: cPixmap(layer, viewPort, drawPort),
	  m_pOglThread(oglThread)
{
	int width = drawPort.IsEmpty() ? viewPort.Width() : drawPort.Width();
	int height = drawPort.IsEmpty() ? viewPort.Height() : drawPort.Height();

	if (width > m_pOglThread->MaxTextureSize() || height > m_pOglThread->MaxTextureSize()) {
		LOGWARNING("openglosd: %s: cannot allocate pixmap of %dpx x %dpx, clipped to %dpx x %dpx!", __FUNCTION__,
			width, height, std::min(width, m_pOglThread->MaxTextureSize()), std::min(height, m_pOglThread->MaxTextureSize()));
		width = std::min(width, m_pOglThread->MaxTextureSize());
		height = std::min(height, m_pOglThread->MaxTextureSize());
	}

	m_pFramebuffer = new cOglFb(width, height, viewPort.Width(), viewPort.Height());

#ifdef GRIDPOINTS
	// Creates a tiny font with height GRIDPOINTSTXTSIZE
	m_pTinyfont = cFont::CreateFont(Setup.FontOsd, GRIDPOINTSTXTSIZE);
#endif
}

cOglPixmap::~cOglPixmap(void)
{
	if (!m_pOglThread->Active())
		return;

	m_pOglThread->DoCmd(new cOglCmdDeleteFb(m_pFramebuffer));
#ifdef GRIDPOINTS
	delete m_pTinyfont;
#endif
}

void cOglPixmap::MarkViewPortDirty(const cRect &rect)
{
	cPixmap::MarkViewPortDirty(rect);
	SetDirty();
}

void cOglPixmap::SetClean(void)
{
	cPixmap::SetClean();
	SetDirty(false);
}

void cOglPixmap::SetLayer(int layer)
{
	cPixmap::SetLayer(layer);
	SetDirty();
}

void cOglPixmap::SetAlpha(int alpha)
{
	alpha = constrain(alpha, ALPHA_TRANSPARENT, ALPHA_OPAQUE);
	if (alpha != cPixmap::Alpha()) {
		cPixmap::SetAlpha(alpha);
		SetDirty();
	}
}

void cOglPixmap::SetTile(bool tile)
{
	cPixmap::SetTile(tile);
	SetDirty();
}

void cOglPixmap::SetViewPort(const cRect &rect)
{
	cPixmap::SetViewPort(rect);
	SetDirty();
}

void cOglPixmap::SetDrawPortPoint(const cPoint &point, bool dirty)
{
	cPixmap::SetDrawPortPoint(point, dirty);
	if (dirty)
		SetDirty();
}

void cOglPixmap::Clear(void)
{
	if (!m_pOglThread->Active())
		return;

	LOCK_PIXMAPS;
	m_pOglThread->DoCmd(new cOglCmdFill(m_pFramebuffer, clrTransparent));
	SetDirty();
	MarkDrawPortDirty(DrawPort());
}

void cOglPixmap::Fill(tColor color)
{
	if (!m_pOglThread->Active())
		return;

	LOCK_PIXMAPS;
	m_pOglThread->DoCmd(new cOglCmdFill(m_pFramebuffer, color));
	SetDirty();
	MarkDrawPortDirty(DrawPort());
}

void cOglPixmap::DrawImage(const cPoint &point, const cImage &image)
{
	DrawScaledImage(point, image);
}

void cOglPixmap::DrawImage(const cPoint &point, int imageHandle)
{
	DrawScaledImage(point, imageHandle);
}

void cOglPixmap::DrawScaledImage(const cPoint &point, const cImage &image, double factorX, double factorY, __attribute__ ((unused)) bool antiAlias)
{
	if (!m_pOglThread->Active())
		return;

	tColor *argb = MALLOC(tColor, image.Width() * image.Height());
	if (!argb)
		return;
	memcpy(argb, image.Data(), sizeof(tColor) * image.Width() * image.Height());

	m_pOglThread->DoCmd(new cOglCmdDrawImage(m_pFramebuffer, argb, image.Width(), image.Height(), point.X(), point.Y(), true, factorX, factorY));
#ifdef GRIDRECT
	DrawGridRect(cRect(point.X(), point.Y(), image.Width() * factorX, image.Height() * factorY), GRIDPOINTOFFSET, GRIDPOINTSIZE, GRIDPOINTCLR, GRIDPOINTBG, m_pTinyfont);
#endif
	SetDirty();
	MarkDrawPortDirty(cRect(point, cSize(image.Width() * factorX, image.Height() * factorY)).Intersected(DrawPort().Size()));
}

void cOglPixmap::DrawScaledImage(const cPoint &point, int imageHandle, double factorX, double factorY, __attribute__ ((unused)) bool antiAlias)
{
	if (!m_pOglThread->Active())
		return;

	if (imageHandle < 0 && m_pOglThread->GetImageRef(imageHandle)) {
			sOglImage *img = m_pOglThread->GetImageRef(imageHandle);
			m_pOglThread->DoCmd(new cOglCmdDrawTexture(m_pFramebuffer, img, point.X(), point.Y(), factorX, factorY));
#ifdef GRIDRECT
			DrawGridRect(cRect(point.X(), point.Y(), img->width * factorX, img->height * factorY), GRIDPOINTOFFSET, GRIDPOINTSIZE, GRIDPOINTCLR, GRIDPOINTBG, m_pTinyfont);
#endif
			SetDirty();
			MarkDrawPortDirty(cRect(point, cSize(img->width * factorX, img->height * factorY)).Intersected(DrawPort().Size()));
	}
}

void cOglPixmap::DrawPixel(const cPoint &point, tColor color)
{
	cRect r(point.X(), point.Y(), 1, 1);
	m_pOglThread->DoCmd(new cOglCmdDrawRectangle(m_pFramebuffer, r.X(), r.Y(), r.Width(), r.Height(), color));
#ifdef GRIDRECT
	DrawGridRect(cRect(r.X(), r.Y(), 0, 0), GRIDPOINTOFFSET, GRIDPOINTSIZE, GRIDPOINTCLR, GRIDPOINTBG, m_pTinyfont);
#endif
	SetDirty();
	MarkDrawPortDirty(r);
}

void cOglPixmap::DrawBitmap(const cPoint &point, const cBitmap &bitmap, tColor colorFg, tColor colorBg, bool overlay)
{
	if (!m_pOglThread->Active())
		return;

	LOCK_PIXMAPS;
	bool specialColors = colorFg || colorBg;
	tColor *argb = MALLOC(tColor, bitmap.Width() * bitmap.Height());
	if (!argb)
		return;

	tColor *p = argb;
	for (int py = 0; py < bitmap.Height(); py++)
		for (int px = 0; px < bitmap.Width(); px++) {
				tIndex index = *bitmap.Data(px, py);
				*p++ = (!index && overlay) ? clrTransparent :
					(specialColors ? (index == 0 ? colorBg : index == 1 ? colorFg :
					bitmap.Color(index)) : bitmap.Color(index));
		}

	m_pOglThread->DoCmd(new cOglCmdDrawImage(m_pFramebuffer, argb, bitmap.Width(), bitmap.Height(), point.X(), point.Y(), true));
#ifdef GRIDRECT
	DrawGridRect(cRect(point.X(), point.Y(), bitmap.Width(), bitmap.Height()), GRIDPOINTOFFSET, GRIDPOINTSIZE, GRIDPOINTCLR, GRIDPOINTBG, m_pTinyfont);
#endif

	SetDirty();
	MarkDrawPortDirty(cRect(cPoint(point.X(), point.Y()), cSize(bitmap.Width(), bitmap.Height())).Intersected(DrawPort().Size()));
}

void cOglPixmap::DrawText(const cPoint &point, const char *s, tColor colorFg, tColor colorBg, const cFont *font, int width, int height, int alignment)
{
	DrawTextInternal(point, s, colorFg, colorBg, font, width, height, alignment, false);
}

#ifdef GRIDPOINTS
void cOglPixmap::DrawGridText(const cPoint &point, const char *s, tColor colorFg, tColor colorBg, const cFont *font, int width, int height, int alignment)
{
	DrawTextInternal(point, s, colorFg, colorBg, font, width, height, alignment, true);
}
#endif

void cOglPixmap::DrawTextInternal(const cPoint &point, const char *s, tColor colorFg, tColor colorBg, const cFont *font, int width, int height, int alignment, bool isGridText)
{
	if (!m_pOglThread->Active())
		return;

	LOCK_PIXMAPS;
	int len = s ? Utf8StrLen(s) : 0;
	unsigned int *symbols = MALLOC(unsigned int, len + 1);
	if (!symbols)
		return;

	if (len)
		Utf8ToArray(s, symbols, len + 1);
	else
		symbols[0] = 0;

	int x = point.X();
	int y = point.Y();
	int w = font->Width(s);
	int h = font->Height();
	int limitX = 0;
	int cw = width ? width : w;
	int ch = height ? height : h;

	// workaround for messages in SkinElchiHD
	if (width > ViewPort().Width() && !x && !isGridText)
		x = ViewPort().Width() - w;

	cRect r(x, y, cw, ch);

	if (colorBg != clrTransparent)
		m_pOglThread->DoCmd(new cOglCmdDrawRectangle(m_pFramebuffer, r.X(), r.Y(), r.Width(), r.Height(), colorBg));

	if (width || height) {
		limitX = x + cw;
		if (width) {
			if ((alignment & taLeft) != 0) {
				if ((alignment & taBorder) != 0)
					x += std::max(h / TEXT_ALIGN_BORDER, 1);
			} else if ((alignment & taRight) != 0) {
				if (w < width)
					x += width - w;
				if ((alignment & taBorder) != 0)
					x -= std::max(h / TEXT_ALIGN_BORDER, 1);
			} else { // taCentered
				if (w < width)
					x += (width - w) / 2;
			}
		}

		if (height) {
			if ((alignment & taTop) != 0)
				;
			else if ((alignment & taBottom) != 0) {
				if (h < height)
					y += height - h;
			} else { // taCentered
				if (h < height)
				y += (height - h) / 2;
			}
		}
	}
	m_pOglThread->DoCmd(new cOglCmdDrawText(m_pFramebuffer, x, y, symbols, limitX, font->FontName(), font->Size(), colorFg, len));

#ifdef GRIDTEXT
	if (!isGridText)
		DrawGridRect(cRect(x, y, cw, ch), GRIDPOINTOFFSET, GRIDPOINTSIZE, GRIDPOINTCLR, GRIDPOINTBG, m_pTinyfont);
#endif

	SetDirty();
	MarkDrawPortDirty(r);
}

void cOglPixmap::DrawRectangle(const cRect &rect, tColor color)
{
	if (!m_pOglThread->Active())
		return;

	LOCK_PIXMAPS;
	m_pOglThread->DoCmd(new cOglCmdDrawRectangle(m_pFramebuffer, rect.X(), rect.Y(), rect.Width(), rect.Height(), color));
#ifdef GRIDRECT
	DrawGridRect(rect, GRIDPOINTOFFSET, GRIDPOINTSIZE, GRIDPOINTCLR, GRIDPOINTBG, m_pTinyfont);
#endif

	SetDirty();
	MarkDrawPortDirty(rect);
}

void cOglPixmap::DrawEllipse(const cRect &rect, tColor color, int quadrants)
{
	if (!m_pOglThread->Active())
		return;

	LOCK_PIXMAPS;
	m_pOglThread->DoCmd(new cOglCmdDrawEllipse(m_pFramebuffer, rect.X(), rect.Y(), rect.Width(), rect.Height(), color, quadrants));
#ifdef GRIDRECT
	DrawGridRect(rect, GRIDPOINTOFFSET, GRIDPOINTSIZE, GRIDPOINTCLR, GRIDPOINTBG, m_pTinyfont);
#endif

	SetDirty();
	MarkDrawPortDirty(rect);
}

void cOglPixmap::DrawSlope(const cRect &rect, tColor color, int type)
{
	if (!m_pOglThread->Active())
		return;

	LOCK_PIXMAPS;
	m_pOglThread->DoCmd(new cOglCmdDrawSlope(m_pFramebuffer, rect.X(), rect.Y(), rect.Width(), rect.Height(), color, type));
#ifdef GRIDRECT
	DrawGridRect(rect, GRIDPOINTOFFSET, GRIDPOINTSIZE, GRIDPOINTCLR, GRIDPOINTBG, m_pTinyfont);
#endif

	SetDirty();
	MarkDrawPortDirty(rect);
}

void cOglPixmap::Render(const cPixmap *pixmap, const cRect &source, const cPoint &dest)
{
	LOGWARNING("openglosd: %s: %d %d %d not implemented in OpenGl OSD", __FUNCTION__, pixmap->ViewPort().X(), source.X(), dest.X());
}

void cOglPixmap::Copy(const cPixmap *pixmap, const cRect &source, const cPoint &dest)
{
	LOGWARNING("openglosd: %s: %d %d %d not implemented in OpenGl OSD", __FUNCTION__, pixmap->ViewPort().X(), source.X(), dest.X());
}

void cOglPixmap::Scroll(const cPoint &dest, const cRect &source)
{
	LOGWARNING("openglosd: %s: %d %d not implemented in OpenGl OSD", __FUNCTION__, source.X(), dest.X());
}

void cOglPixmap::Pan(const cPoint &dest, const cRect &source)
{
	LOGWARNING("openglosd: %s: %d %d not implemented in OpenGl OSD", __FUNCTION__, source.X(), dest.X());
}

#ifdef GRIDPOINTS
void cOglPixmap::DrawGridRect(const cRect &rect, int offset, int size, tColor clr, tColor bg, const cFont *font)
{
	int x1 = rect.X() + offset;
	int x2 = rect.X() + rect.Width() + offset;
	int y1 = rect.Y();
	int y2 = rect.Y() + rect.Height();
	char p1[10];
	char p2[10];
	char p3[10];
	char p4[10];
	sprintf(p1, "%d.%d", x1, y1);
	sprintf(p2, "%d.%d", x2, y1);
	sprintf(p3, "%d.%d", x1, y2);
	sprintf(p4, "%d.%d", x2, y2);

	m_pOglThread->DoCmd(new cOglCmdDrawRectangle(m_pFramebuffer, x1, y1, size, size, clr));
#ifdef GRIDPOINTSTEXT
	DrawGridText(cPoint(x1, y1), p1, clr, bg, font);
#endif
	if (Rect.Width() && Rect.Height()) {
		m_pOglThread->DoCmd(new cOglCmdDrawRectangle(m_pFramebuffer, x2, y1, size, size, clr));
		m_pOglThread->DoCmd(new cOglCmdDrawRectangle(m_pFramebuffer, x1, y2, size, size, clr));
		m_pOglThread->DoCmd(new cOglCmdDrawRectangle(m_pFramebuffer, x2, y2, size, size, clr));
#ifdef GRIDPOINTSTEXT
		DrawGridText(cPoint(x2, y1), p2, clr, bg, font);
		DrawGridText(cPoint(x1, y2), p3, clr, bg, font);
		DrawGridText(cPoint(x2, y2), p4, clr, bg, font);
#endif
	}
}
#endif

/******************************************************************************
* cOglOsd
******************************************************************************/
cOglOutputFb *cOglOsd::OutputFramebuffer = NULL;

cOglOsd::cOglOsd(int left, int top, uint level, std::shared_ptr<cOglThread> oglThread, cSoftHdDevice *device)
	: cOsd(left, top, level),
	  m_pOglThread(oglThread),
	  m_isSubtitleOsd(level == 10 ? true : false),
	  m_pDevice(device)
{
	int osdWidth = 0;
	int osdHeight = 0;
	double pixelAspect;
	m_pDevice->GetOsdSize(osdWidth, osdHeight, pixelAspect);
	LOGDEBUG2(L_OSD, "openglosd: %s: New Osd %p osdLeft %d osdTop %d screenWidth %d screenHeight %d", __FUNCTION__, this, left, top, osdWidth, osdHeight);

	m_maxPixmapSize.Set(m_pOglThread->MaxTextureSize(), m_pOglThread->MaxTextureSize());

	if (!OutputFramebuffer) {
		OutputFramebuffer = new cOglOutputFb(osdWidth, osdHeight);
		m_pOglThread->DoCmd(new cOglCmdInitOutputFb(OutputFramebuffer));
	}
}

cOglOsd::~cOglOsd()
{
	if (!m_pOglThread->Active() || !Active() || !m_pBufferFramebuffer)
		return;

	LOGDEBUG2(L_OSD, "openglosd: %s: Delete Osd %p", __FUNCTION__, this);
	m_pOglThread->DoCmd(new cOglCmdFill(m_pBufferFramebuffer, clrTransparent));

	SetActive(false); // OsdClose() is done in cOglCmdCopyBufferToOutputFb()
	m_pOglThread->DoCmd(new cOglCmdCopyBufferToOutputFb(m_pBufferFramebuffer, OutputFramebuffer, Left(), Top(), 0, m_pDevice));
	m_pOglThread->DoCmd(new cOglCmdDeleteFb(m_pBufferFramebuffer));
}

eOsdError cOglOsd::SetAreas(const tArea *areas, int numAreas)
{
	cRect r;
	if (numAreas > 1)
		m_isSubtitleOsd = true;
	for (int i = 0; i < numAreas; i++)
		r.Combine(cRect(areas[i].x1, areas[i].y1, areas[i].Width(), areas[i].Height()));

	tArea area = { r.Left(), r.Top(), r.Right(), r.Bottom(), 32 };

	// now we know the actual osd size, create double buffer frame buffer
	if (m_pBufferFramebuffer) {
		m_pOglThread->DoCmd(new cOglCmdDeleteFb(m_pBufferFramebuffer));
		DestroyPixmap(m_pOglPixmaps[0]);
	}
	m_pBufferFramebuffer = new cOglFb(r.Width(), r.Height(), r.Width(), r.Height());
	cCondWait initiated;
	m_pOglThread->DoCmd(new cOglCmdInitFb(m_pBufferFramebuffer, &initiated));
	initiated.Wait();

	return cOsd::SetAreas(&area, 1);
}

cPixmap *cOglOsd::CreatePixmap(int layer, const cRect &viewPort, const cRect &drawPort)
{
	if (!m_pOglThread->Active())
		return NULL;

	LOCK_PIXMAPS;
	cOglPixmap *p = new cOglPixmap(m_pOglThread, layer, viewPort, drawPort);
	if (cOsd::AddPixmap(p)) {
		// find a free slot
		for (int i = 0; i < m_pOglPixmaps.Size(); i++) {
			if (!m_pOglPixmaps[i])
				return m_pOglPixmaps[i] = p;
		}
		m_pOglPixmaps.Append(p);
		return p;
	}
	delete p;

	return NULL;
}

void cOglOsd::DestroyPixmap(cPixmap *Pixmap)
{
	if (!m_pOglThread->Active())
		return;
	if (!Pixmap)
		return;

	LOCK_PIXMAPS;
	int start = 1;
	if (m_isSubtitleOsd)
		start = 0;
	for (int i = start; i < m_pOglPixmaps.Size(); i++) {
		if (m_pOglPixmaps[i] == Pixmap) {
			if (Pixmap->Layer() >= 0)
				m_pOglPixmaps[0]->MarkViewPortDirty(m_pOglPixmaps[i]->ViewPort());

			m_pOglPixmaps[i] = NULL;
			if (i)
				cOsd::DestroyPixmap(Pixmap);

			return;
		}
	}
}

void cOglOsd::Flush(void)
{
	if (!m_pOglThread->Active() || !Active())
		return;

	LOGDEBUG2(L_OSD, "openglosd: %s: Flush Osd %p", __FUNCTION__, this);
	LOCK_PIXMAPS;
	// check for dirty areas
	m_pDirtyViewport.Set(0, 0, 0, 0);
	for (int i = 0; i < m_pOglPixmaps.Size(); i++) {
		if (m_pOglPixmaps[i] && m_pOglPixmaps[i]->IsDirty()) {
			if (m_isSubtitleOsd)
				m_pDirtyViewport.Combine(m_pOglPixmaps[i]->DirtyViewPort().Size());
			else
				m_pDirtyViewport.Combine(m_pOglPixmaps[i]->DirtyViewPort());

			m_pOglPixmaps[i]->SetClean();
		}
	}

	if (m_pDirtyViewport.IsEmpty())
		return;

	// clear private buffer within the dirty area
	m_pOglThread->DoCmd(new cOglCmdDrawRectangle(m_pBufferFramebuffer,
	                                             m_pDirtyViewport.X(),
	                                             m_pDirtyViewport.Y(),
	                                             m_pDirtyViewport.Width(),
	                                             m_pDirtyViewport.Height(),
	                                             clrTransparent));

	// render pixmap textures blended to private buffer
	for (int layer = 0; layer < MAXPIXMAPLAYERS; layer++) {
		for (int i = 0; i < m_pOglPixmaps.Size(); i++) {
			if (!m_pOglPixmaps[i])
				continue;

			if (m_pOglPixmaps[i]->Layer () != layer)
				continue;

			if (m_isSubtitleOsd && !m_pDirtyViewport.Intersects(m_pOglPixmaps[i]->ViewPort().Size()))
				continue;

			if (!m_isSubtitleOsd && !m_pDirtyViewport.Intersects(m_pOglPixmaps[i]->ViewPort()))
				continue;

			bool alphablending = layer == 0 ? false : true; // Decide wether to render (with alpha) or copy a pixmap
			m_pOglThread->DoCmd(new cOglCmdRenderFbToBufferFb(m_pOglPixmaps[i]->Framebuffer(),
			                                                  m_pBufferFramebuffer,
			                                                  m_isSubtitleOsd ? 0 : m_pOglPixmaps[i]->ViewPort().X(),
			                                                  m_isSubtitleOsd ? 0 : m_pOglPixmaps[i]->ViewPort().Y(),
			                                                  m_pOglPixmaps[i]->Alpha(),
			                                                  m_pOglPixmaps[i]->DrawPort().X(),
			                                                  m_pOglPixmaps[i]->DrawPort().Y(),
			                                                  m_pDirtyViewport.X(),
			                                                  m_pDirtyViewport.Top(),
			                                                  m_pDirtyViewport.Width(),
			                                                  m_pDirtyViewport.Height(),
			                                                  alphablending,
			                                                  m_pDevice));
		}
	}
	// copy the private buffer to output framebuffer
	m_pOglThread->DoCmd(new cOglCmdBufferFill(OutputFramebuffer, clrTransparent));

	m_pOglThread->DoCmd(new cOglCmdCopyBufferToOutputFb(m_pBufferFramebuffer, OutputFramebuffer,
	                                                    Left() + (m_isSubtitleOsd ? m_pOglPixmaps[0]->ViewPort().X() : 0),
	                                                    Top() + (m_isSubtitleOsd ? m_pOglPixmaps[0]->ViewPort().Y() : 0), 1, m_pDevice));
}

void cOglOsd::DrawScaledBitmap(int x, int y, const cBitmap &Bitmap, double FactorX, double FactorY, bool AntiAlias)
{
	if (!m_pOglPixmaps[0])
		return;

	std::unique_ptr<cBitmap> scaledBitmap;
	const cBitmap *b = &Bitmap;

	if (!DoubleEqual(FactorX, 1.0) || !DoubleEqual(FactorY, 1.0)) {
		scaledBitmap.reset(Bitmap.Scaled(FactorX, FactorY, AntiAlias));
		b = scaledBitmap.get();
	}

	int xNew = x;
	int yNew = y;

	const cRect &viewport = m_pOglPixmaps[0]->ViewPort();
	if (m_isSubtitleOsd && (x >= viewport.X()))
		xNew -= viewport.X();
	if (m_isSubtitleOsd && (y >= viewport.Y()))
		yNew -= viewport.Y();

	m_pOglPixmaps[0]->DrawBitmap(cPoint(xNew, yNew), *b);
}
