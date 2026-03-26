/**
 * @file openglosd.h
 * Osd class - hardware accelerated (OpenGL/ES) - header file
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

#ifndef __SOFTHDDEVICE_OPENGLOSD_H
#define __SOFTHDDEVICE_OPENGLOSD_H

#include <cstdio>
#include <memory>
#include <mutex>
#include <queue>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_LCD_FILTER_H
#include FT_STROKER_H

#undef __FTERRORS_H__
#define FT_ERRORDEF( e, v, s )  { e, s },
#define FT_ERROR_START_LIST     {
#define FT_ERROR_END_LIST       { 0, 0 } };
const struct {
	int          code;
	const char*  message;
} FT_Errors[] =
#include FT_ERRORS_H

#include <GLES2/gl2.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <vdr/osd.h>

// This is needed for the GLES2 GL_CLAMP_TO_BORDER workaround
#define BORDERCOLOR         0x00000000

struct sOglImage {
	GLuint texture;
	GLint width;
	GLint height;
	bool used;
};

class cSoftHdDevice;
class cVideoRender;

/****************************************************************************************
 * cOglShader
 *
 * Represents a shader and keeps everything we need to set variable within the shader.
 ***************************************************************************************/
enum eShaderType {
	stRect,
	stTexture,
	stTextureSwapBR,
	stText,
	stCount
};

class cOglShader
{
public:
	cOglShader(void) {};

	bool Load(eShaderType);
	void Use(void);
	void SetFloat    (const GLchar *, GLfloat);
	void SetInteger  (const GLchar *, GLint);
	void SetVector2f (const GLchar *, GLfloat, GLfloat);
	void SetVector3f (const GLchar *, GLfloat, GLfloat, GLfloat);
	void SetVector4f (const GLchar *, GLfloat, GLfloat, GLfloat, GLfloat);
	void SetMatrix4  (const GLchar *, const glm::mat4 &);

private:
	eShaderType m_type;
	GLuint m_id;

	bool Compile(const char *, const char *);
	bool CheckCompileErrors(GLuint, bool program = false);
};

/****************************************************************************************
 * cOglGlyph
 *
 * Represents a single glyph of a font.
 ***************************************************************************************/
class cOglGlyph : public cListObject
{
public:
	cOglGlyph(FT_ULong, FT_BitmapGlyph);
	virtual ~cOglGlyph();

	FT_ULong CharCode(void) { return m_charCode; }
	int AdvanceX(void) { return m_advanceX; }
	int BearingLeft(void) const { return m_bearingLeft; }
	int BearingTop(void) const { return m_bearingTop; }
	int Width(void) const { return m_width; }
	int Height(void) const { return m_height; }
	int GetKerningCache(FT_ULong);
	void SetKerningCache(FT_ULong, int);
	void LoadTexture(void);
	void BindTexture(void);
protected:
	struct tKerning {
		public:
			tKerning(FT_ULong prevSym, GLfloat kerning = 0.0f)  {
				this->prevSym = prevSym;
				this->kerning = kerning;
			}
			FT_ULong prevSym;
			GLfloat kerning;
	};
	FT_ULong m_charCode;
	int m_bearingLeft;
	int m_bearingTop;
	int m_width;
	int m_height;
	unsigned char *m_pBuffer;
	int m_advanceX;
	cVector<tKerning> m_pKerningCache;
	GLuint m_texture = 0;
};

/****************************************************************************************
 * cOglAtlasGlyph
 *
 * A glyph a font-atlas (texture-atlas) needs some more infos like offset on the texture.
 ***************************************************************************************/
class cOglAtlasGlyph : public cOglGlyph
{
public:
	cOglAtlasGlyph(FT_ULong charCode, FT_BitmapGlyph ftGlyph, float offsetX, float offsetY)
		: cOglGlyph(charCode, ftGlyph),
		  m_advanceY(ftGlyph->root.advance.y >> 16),   // value in 1/2^16 pixel
		  m_offsetX(offsetX),
		  m_offsetY(offsetY) {};

	int AdvanceY(void) { return m_advanceY; }
	float OffsetX(void) const { return m_offsetX; }
	float OffsetY(void) const { return m_offsetY; }
private:
	int m_advanceY;
	float m_offsetX;
	float m_offsetY;
};

/****************************************************************************************
 * cOglFontAtlas
 *
 * Represents a texture atlas keeping a range of glyphs on one texture per font and size
 * instead of having one texture per glyph. This technique makes dealing with huge
 * amounts of glyphs faster, because the bottleneck (texture up-/download/binding) is
 * reduced at a minimum. Its faster to deal with one single bigger texture than many
 * smaller ones.
 *
 * The font atlas is prepared once at the time the new font or sized is accessed for the
 * first time. We may have a little delay at startup, which is negligible.
 ****************************************************************************************/
#define MIN_CHARCODE 32
#define MAX_CHARCODE 255
class cOglFontAtlas
{
public:
	cOglFontAtlas(FT_Face, int);
	virtual ~cOglFontAtlas(void);
	cOglAtlasGlyph* GetGlyph(int) const;
	int Height(void) const { return m_height; }
	int Width(void) const { return m_width; }
	void BindTexture(void);
private:
	GLuint m_texture = 0;
	int m_width = 0;
	int m_height = 0;
	cOglAtlasGlyph* m_pGlyph[MAX_CHARCODE - MIN_CHARCODE + 1];
};

/****************************************************************************************
 * cOglFont
 *
 * Represents a OSD font (one per size and font family)
 ***************************************************************************************/
class cOglFont : public cListObject
{
public:
	virtual ~cOglFont(void);
	static cOglFont *Get(const char *, int);
	cOglFontAtlas *Atlas(void) { return m_pAtlas; };
	static void Cleanup(void);
	const char *Name(void) { return *m_name; };
	int Size(void) { return m_size; };
	int Bottom(void) {return m_bottom; };
	int Height(void) {return m_height; };
	cOglGlyph* Glyph(FT_ULong) const;
	int Kerning(cOglGlyph *glyph, FT_ULong prevSym) const;
private:
	static bool s_initiated;
	static FT_Library s_ftLib;
	static cList<cOglFont> *s_pFonts;

	cString m_name;
	int m_size;
	int m_height = 0;
	int m_bottom = 0;
	FT_Face m_face;
	mutable cList<cOglGlyph> m_glyphCache;
	cOglFontAtlas *m_pAtlas;

	cOglFont(const char *, int);
	static void Init(void);
};

/****************************************************************************************
 * cOglFb
 *
 * A framebuffer object which can be rendered onto (pixmap)
 ****************************************************************************************/
class cOglFb
{
public:
	cOglFb(GLint, GLint, GLint, GLint);
	virtual ~cOglFb(void);

	bool Initiated(void) { return m_initiated; }
	virtual bool Init(void);
	void Bind(void);
	virtual void Unbind(void);
	bool BindTexture(void);
	void Blit(GLint, GLint, GLint, GLint);
	GLint Width(void) { return m_width; };
	GLint Height(void) { return m_height; };
	bool Scrollable(void) { return m_scrollable; };
	GLint ViewportWidth(void) { return m_viewPortWidth; };
	GLint ViewportHeight(void) { return m_viewPortHeight; };
protected:
	bool m_initiated = false;
	GLint m_width, m_height;
private:
	GLuint m_framebuffer = 0;
	GLuint m_texture = 0;
	GLint m_viewPortWidth, m_viewPortHeight;
	bool m_scrollable = false;
};

/****************************************************************************************
 * cOglOutputFb
 *
 * Output Framebuffer Object - holds the texture which is our "OSD output framebuffer"
 ***************************************************************************************/
class cOglOutputFb : public cOglFb
{
public:
	cOglOutputFb(GLint width, GLint height) : cOglFb(width, height, width, height) {};

	virtual bool Init(void);
	virtual void Unbind(void);
private:
	GLuint m_framebuffer = 0;
	GLuint m_texture = 0;
};

/****************************************************************************************
 * cOglVb
 *
 * Describes and handles the OpenGL vertices for the different drawing commands
 ***************************************************************************************/
enum eVertexBufferType {
	vbRect,
	vbEllipse,
	vbSlope,
	vbTexture,
	vbTextureSwapBR,
	vbText,
	vbCount
};

class cOglVb
{
public:
	cOglVb(int type) : m_type((eVertexBufferType)type) {};
	virtual ~cOglVb(void) {};

	bool Init(void);
	void Bind(void);
	void Unbind(void);
	void ActivateShader(void);
	void EnableBlending(void);
	void DisableBlending(void);
	void SetShaderColor(GLint);
	void SetShaderBorderColor(GLint);
	void SetShaderTexture(GLint);
	void SetShaderAlpha(GLint);
	void SetShaderProjectionMatrix(GLint, GLint);
	void SetVertexSubData(GLfloat *, int count = 0);
	void SetVertexData(GLfloat *, int count = 0);
	void DrawArrays(int count = 0);
private:
	eVertexBufferType m_type;
	eShaderType m_shader;
	GLuint m_vao;
	GLuint m_vbo = 0;
	GLuint m_positionLoc = 0;
	GLuint m_texCoordsLoc = 1;
	int m_sizeVertex1 = 0;
	int m_sizeVertex2 = 0;
	int m_numVertices = 0;
	GLuint m_drawMode = 0;
};

/****************************************************************************************
 * cOglCmd and derived classes
 *
 * Every draw action is transposed to one of the following cOglCmd* methods,
 * which are sent to the command queue, executed by cOglThread.
 ***************************************************************************************/
class cOglCmd
{
public:
	cOglCmd(cOglFb *fb)
		: m_pFramebuffer(fb) {};
	virtual ~cOglCmd(void) {};
	virtual const char* Description(void) = 0;
	virtual bool Execute(void) = 0;
	virtual bool NeedsLockingAgainstStateChange(void) { return false; };
protected:
	cOglFb *m_pFramebuffer;
};

class cOglCmdInitOutputFb : public cOglCmd
{
public:
	cOglCmdInitOutputFb(cOglOutputFb *oFb)
		: cOglCmd(NULL),
		  m_pOutputFramebuffer(oFb) {};
	virtual ~cOglCmdInitOutputFb(void) {};
	virtual const char* Description(void) { return "InitOutputFramebuffer"; }
	virtual bool Execute(void);
private:
	cOglOutputFb *m_pOutputFramebuffer;
};

class cOglCmdInitFb : public cOglCmd
{
public:
	cOglCmdInitFb(cOglFb *fb, cCondWait *wait = NULL)
		: cOglCmd(fb),
		  m_wait(wait) {};
	virtual ~cOglCmdInitFb(void) {};
	virtual const char* Description(void) { return "InitFramebuffer"; }
	virtual bool Execute(void);
private:
	cCondWait *m_wait;
};

class cOglCmdDeleteFb : public cOglCmd
{
public:
	cOglCmdDeleteFb(cOglFb *fb)
		: cOglCmd(fb) {};
	virtual ~cOglCmdDeleteFb(void) {};
	virtual const char* Description(void) { return "DeleteFramebuffer"; }
	virtual bool Execute(void);
};

class cOglCmdRenderFbToBufferFb : public cOglCmd
{
public:
	cOglCmdRenderFbToBufferFb(cOglFb *fb, cOglFb *buffer, GLint x, GLint y, GLint transparency, GLint drawPortX, GLint drawPortY, GLint dirtyX, GLint dirtyTop, GLint dirtyWidth, GLint dirtyHeight, bool alphablending)
		: cOglCmd(fb),
		  m_pBuffer(buffer),
		  m_x((GLfloat)x),
		  m_y((GLfloat)y),
		  m_drawPortX((GLfloat)drawPortX),
		  m_drawPortY((GLfloat)drawPortY),
		  m_transparency((alphablending ? transparency : ALPHA_OPAQUE)),
		  m_bcolor(BORDERCOLOR),
		  m_dirtyX(dirtyX),
		  m_dirtyTop(dirtyTop),
		  m_dirtyWidth(dirtyWidth),
		  m_dirtyHeight(dirtyHeight),
		  m_alphablending(alphablending) {};
	virtual ~cOglCmdRenderFbToBufferFb(void) {};
	virtual const char* Description(void) { return "Render Framebuffer to Buffer"; }
	virtual bool Execute(void);
private:
	cOglFb *m_pBuffer;
	GLfloat m_x, m_y;
	GLfloat m_drawPortX, m_drawPortY;
	GLint m_transparency;
	GLint m_bcolor;
	GLint m_dirtyX;
	GLint m_dirtyTop;
	GLint m_dirtyWidth;
	GLint m_dirtyHeight;
	bool m_alphablending;
};

class cOglCmdCopyBufferToOutputFb : public cOglCmd
{
public:
	cOglCmdCopyBufferToOutputFb(cOglFb *fb, cOglOutputFb *oFb, GLint x, GLint y, int active, cSoftHdDevice *device)
		: cOglCmd(fb),
		  m_pOutputFramebuffer(oFb),
		  m_x((GLfloat)x),
		  m_y((GLfloat)y),
		  m_borderColor(BORDERCOLOR),
		  m_active(active),
		  m_pDevice(device) {};
	virtual ~cOglCmdCopyBufferToOutputFb(void) {};
	virtual const char* Description(void) { return "Copy buffer to OutputFramebuffer"; }
	virtual bool Execute(void);
	bool NeedsLockingAgainstStateChange(void) { return true; };
private:
	cOglOutputFb *m_pOutputFramebuffer;
	GLfloat m_x, m_y;
	GLint m_borderColor;
	int m_active;
	cSoftHdDevice *m_pDevice;
};

class cOglCmdFill : public cOglCmd
{
public:
	cOglCmdFill(cOglFb *fb, GLint color)
		: cOglCmd(fb),
		  m_color(color) {};
	virtual ~cOglCmdFill(void) {};
	virtual const char* Description(void) { return "Fill"; }
	virtual bool Execute(void);
private:
	GLint m_color;
};

class cOglCmdBufferFill : public cOglCmd
{
public:
	cOglCmdBufferFill(cOglFb *fb, GLint color)
		: cOglCmd(fb),
		  m_color(color) {};
	virtual ~cOglCmdBufferFill(void) {};
	virtual const char* Description(void) { return "Fill Buffer  "; }
	virtual bool Execute(void);
private:
	GLint m_color;
};

class cOglCmdDrawRectangle : public cOglCmd
{
public:
	cOglCmdDrawRectangle( cOglFb *fb, GLint x, GLint y, GLint width, GLint height, GLint color)
		: cOglCmd(fb),
		  m_x(x),
		  m_y(y),
		  m_width(width),
		  m_height(height),
		  m_color(color) {};
	virtual ~cOglCmdDrawRectangle(void) {};
	virtual const char* Description(void) { return "DrawRectangle"; }
	virtual bool Execute(void);
private:
	GLint m_x, m_y;
	GLint m_width, m_height;
	GLint m_color;
};

class cOglCmdDrawEllipse : public cOglCmd
{
public:
	cOglCmdDrawEllipse( cOglFb *fb, GLint x, GLint y, GLint width, GLint height, GLint color, GLint quadrants)
		: cOglCmd(fb),
		  m_x(x),
		  m_y(y),
		  m_width(width),
		  m_height(height),
		  m_color(color),
		  m_quadrants(quadrants) {};
	virtual ~cOglCmdDrawEllipse(void) {};
	virtual const char* Description(void) { return "DrawEllipse  "; }
	virtual bool Execute(void);
private:
	GLint m_x, m_y;
	GLint m_width, m_height;
	GLint m_color;
	GLint m_quadrants;

	GLfloat *CreateVerticesFull(int &);
	GLfloat *CreateVerticesQuadrant(int &);
	GLfloat *CreateVerticesHalf(int &);
};

class cOglCmdDrawSlope : public cOglCmd
{
public:
	cOglCmdDrawSlope( cOglFb *fb, GLint x, GLint y, GLint width, GLint height, GLint color, GLint type)
		: cOglCmd(fb),
		  m_x(x),
		  m_y(y),
		  m_width(width),
		  m_height(height),
		  m_color(color),
		  m_type(type) {};
	virtual ~cOglCmdDrawSlope(void) {};
	virtual const char* Description(void) { return "DrawSlope    "; }
	virtual bool Execute(void);
private:
	GLint m_x, m_y;
	GLint m_width, m_height;
	GLint m_color;
	GLint m_type;
};

class cOglCmdDrawText : public cOglCmd
{
public:
	cOglCmdDrawText(cOglFb *fb, GLint x, GLint y, unsigned int *symbols, GLint limitX, const char *name, int fontSize, tColor colorText, int length)
		: cOglCmd(fb),
		  m_x(x),
		  m_y(y),
		  m_limitX(limitX),
		  m_colorText(colorText),
		  m_length(length),
		  m_fontName(name),
		  m_fontSize(fontSize),
		  m_pSymbols(symbols) {};
	virtual ~cOglCmdDrawText(void) { free(m_pSymbols); };
	virtual const char* Description(void) { return "DrawText     "; }
	virtual bool Execute(void);
private:
	GLint m_x, m_y;
	GLint m_limitX;
	GLint m_colorText;
	int m_length;
	cString m_fontName;
	int m_fontSize;
	unsigned int *m_pSymbols;
};

class cOglCmdDrawImage : public cOglCmd
{
public:
	cOglCmdDrawImage(cOglFb *fb, tColor *argb, GLint width, GLint height, GLint x, GLint y, bool overlay = true, double scaleX = 1.0f, double scaleY = 1.0f)
		: cOglCmd(fb),
		  m_argb(argb),
		  m_x(x),
		  m_y(y),
		  m_width(width),
		  m_height(height),
		  m_overlay(overlay),
		  m_scaleX(scaleX),
		  m_scaleY(scaleY),
		  m_borderColor(BORDERCOLOR) {};
	virtual ~cOglCmdDrawImage(void) { free(m_argb); };
	virtual const char* Description(void) { return "Draw Image"; }
	virtual bool Execute(void);
private:
	tColor *m_argb;
	GLint m_x, m_y, m_width, m_height;
	bool m_overlay;
	GLfloat m_scaleX, m_scaleY;
	GLint m_borderColor;
};

class cOglCmdDrawTexture : public cOglCmd
{
public:
	cOglCmdDrawTexture(cOglFb *fb, sOglImage *imageRef, GLint x, GLint y, double scaleX = 1.0f, double scaleY = 1.0f)
		: cOglCmd(fb),
		  m_pImageRef(imageRef),
		  m_x(x),
		  m_y(y),
		  m_scaleX(scaleX),
		  m_scaleY(scaleY),
		  m_borderColor(BORDERCOLOR) {};
	virtual ~cOglCmdDrawTexture(void) {};
	virtual const char* Description(void) { return "Draw Texture"; }
	virtual bool Execute(void);
private:
	sOglImage *m_pImageRef;
	GLint m_x, m_y;
	GLfloat m_scaleX, m_scaleY;
	GLint m_borderColor;
};

class cOglCmdStoreImage : public cOglCmd
{
public:
	cOglCmdStoreImage(sOglImage *imageRef, tColor *argb)
		: cOglCmd(NULL),
		  m_pImageRef(imageRef),
		  m_pData(argb) {};
	virtual ~cOglCmdStoreImage(void) { free(m_pData); };
	virtual const char* Description(void) { return "Store Image"; }
	virtual bool Execute(void);
private:
	sOglImage *m_pImageRef;
	tColor *m_pData;
};

class cOglCmdDropImage : public cOglCmd
{
public:
	cOglCmdDropImage(sOglImage *imageRef, cCondWait *wait)
		: cOglCmd(NULL),
		  m_pImageRef(imageRef),
		  m_pWait(wait) {};
	virtual ~cOglCmdDropImage(void) {};
	virtual const char* Description(void) { return "Drop Image"; }
	virtual bool Execute(void);
private:
	sOglImage *m_pImageRef;
	cCondWait *m_pWait;
};

/******************************************************************************
 * cOglThread
 *
 * Every OSD draw or flush which is invoked by VDR is transposed into an
 * OpenGL command.
 * cOglThread holds a fifo-queue of these commands. It continuosly checks
 * for commands on the queue, pops them and sends them to the hardware.
 *
 * On startup it initiates all necessary OpenGL bits.
 *****************************************************************************/
#define OGL_MAX_OSDIMAGES 512
#define OGL_CMDQUEUE_SIZE 200

class cOglThread : public cThread
{
public:
	cOglThread(cCondWait *startWait, int maxCacheSize, cSoftHdDevice *device);
	virtual ~cOglThread(void) {};

	void RequestStop(void);
	void Stop(void);
	void DoCmd(cOglCmd*);
	int StoreImage(const cImage &);
	void DropImageData(int);
	sOglImage *GetImageRef(int);
	int MaxTextureSize(void) { return m_maxTextureSize; };
	void LockOutputFb(void) { m_mutex.lock(); };
	void UnlockOutputFb(void) { m_mutex.unlock(); };
protected:
	virtual void Action(void);
private:
	cCondWait *m_startWait;
	cCondWait m_wait;
	bool m_stalled = false;
	std::queue<cOglCmd*> m_commands;
	GLint m_maxTextureSize = 0;
	sOglImage m_imageCache[OGL_MAX_OSDIMAGES];
	long m_memCached = 0;
	long m_maxCacheSize;
	cVideoRender *m_pRender;
	std::mutex m_mutex;

	bool InitOpenGL(void);
	bool InitShaders(void);
	void DeleteShaders(void);
	bool InitVertexBuffers(void);
	void DeleteVertexBuffers(void);
	void Cleanup(void);
	void CleanupImageCache(void);
	int GetFreeSlot(void);
	void ClearSlot(int slot);
	void eglAcquireContext(void);
};

/****************************************************************************************
 * cOglPixmap
 *
 * OpenGL implementation of a cPixmap
 ***************************************************************************************/
class cOglPixmap : public cPixmap
{
public:
	cOglPixmap(std::shared_ptr<cOglThread>, int, const cRect &, const cRect &DrawPort = cRect::Null);
	virtual ~cOglPixmap(void);

	cOglFb *Framebuffer(void) { return m_pFramebuffer; };
	int X(void) { return ViewPort().X(); };
	int Y(void) { return ViewPort().Y(); };
	virtual bool IsDirty(void) { return m_dirty; }
	virtual void SetDirty(bool dirty = true) { m_dirty = dirty; }
	virtual void SetLayer(int);
	virtual void SetAlpha(int);
	virtual void SetTile(bool);
	virtual void SetViewPort(const cRect &);
	virtual void SetDrawPortPoint(const cPoint &, bool Dirty = true);
	virtual void Clear(void);
	virtual void Fill(tColor);
	virtual void DrawImage(const cPoint &, const cImage &);
	virtual void DrawImage(const cPoint &, int);
	virtual void DrawScaledImage(const cPoint &, const cImage &, double FactorX = 1.0f, double FactorY = 1.0f, bool AntiAlias = false);
	virtual void DrawScaledImage(const cPoint &, int, double FactorX = 1.0f, double FactorY = 1.0f, bool AntiAlias = false);
	virtual void DrawPixel(const cPoint &, tColor);
	virtual void DrawBitmap(const cPoint &, const cBitmap &, tColor ColorFg = 0, tColor ColorBg = 0, bool Overlay = false);
	virtual void DrawText(const cPoint &, const char *, tColor, tColor, const cFont *, int Width = 0, int Height = 0, int Alignment = taDefault);
	virtual void DrawRectangle(const cRect &, tColor);
	virtual void DrawEllipse(const cRect &, tColor, int Quadrants = 0);
	virtual void DrawSlope(const cRect &, tColor, int);
	virtual void Render(const cPixmap *, const cRect &, const cPoint &);
	virtual void Copy(const cPixmap *, const cRect &, const cPoint &);
	virtual void Scroll(const cPoint &, const cRect &Source = cRect::Null);
	virtual void Pan(const cPoint &, const cRect &Source = cRect::Null);
	virtual void MarkViewPortDirty(const cRect &);
	virtual void SetClean(void);
private:
	cOglFb *m_pFramebuffer;    ///< everything is drawn onto this framebuffer (one per pixmap)
	std::shared_ptr<cOglThread> m_pOglThread;
	bool m_dirty = true;       ///< true, if there was draw activity on the pixmap
#ifdef GRIDPOINTS
	cFont *m_pTinyfont;

	void DrawGridRect(const cRect &, int, int, tColor, tColor, const cFont *);
	void DrawGridText(const cPoint &, const char *, tColor, tColor, const cFont *, int Width = 0, int Height = 0, int Alignment = taDefault);
#endif
	void DrawTextInternal(const cPoint &, const char *, tColor, tColor, const cFont *, int Width = 0, int Height = 0, int Alignment = taDefault, bool isGridText = false);
};

/******************************************************************************
 * cOglOsd
 *
 * OpenGL implementation of a cOsd
 *****************************************************************************/
class cOglOsd : public cOsd
{
public:
	cOglOsd(int, int, uint, std::shared_ptr<cOglThread>, cSoftHdDevice *);
	virtual ~cOglOsd();

	virtual eOsdError SetAreas(const tArea *, int);
	virtual cPixmap *CreatePixmap(int, const cRect &, const cRect &DrawPort = cRect::Null);
	virtual void DestroyPixmap(cPixmap *);
	virtual void Flush(void);
	virtual const cSize &MaxPixmapSize(void) const { return m_maxPixmapSize; };
	virtual void DrawScaledBitmap(int, int, const cBitmap &, double, double, bool AntiAlias = false);

	static cOglOutputFb *OutputFramebuffer;        ///< main OSD output framebuffer - this keeps our finished "OSD" (one per OSD)
private:
	cOglFb *m_pBufferFramebuffer = nullptr;        ///< all pixmaps are composed onto this framebuffer after each other,
	                                               ///< before this one is blit onto the OSD output framebuffer
	std::shared_ptr<cOglThread> m_pOglThread;      ///< pointer to thread, which executes the commands
	cVector<cOglPixmap *> m_pOglPixmaps;           ///< array of pixmaps
	bool m_isSubtitleOsd;                          ///< true, if this is a subtitle osd
	cSize m_maxPixmapSize;                         ///< maximum allowed size of a pixmap (depends on the maximum OpenGL texture size)
	cRect m_pDirtyViewport;                        ///< the dirty viewport
	cSoftHdDevice *m_pDevice;                      ///< pointer to cSofthdDevice
};

#endif
