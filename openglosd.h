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

struct sOglImage {
	GLuint texture;
	GLint width;
	GLint height;
	bool used;
};

class cVideoRender;
class cSoftHdDevice;

/****************************************************************************************
* Helpers
****************************************************************************************/

void ConvertColor(const GLint &colARGB, glm::vec4 &col);

/****************************************************************************************
* cShader
****************************************************************************************/
enum eShaderType {
	stRect,
	stTexture,
	stTextureSwapBR,
	stText,
	stCount
};

class cShader {
private:
	eShaderType type;
	GLuint id;
	bool Compile(const char *vertexCode, const char *fragmentCode);
	bool CheckCompileErrors(GLuint object, bool program = false);
public:
	cShader(void) {};
	virtual ~cShader(void) {};
	bool Load(eShaderType type);
	void Use(void);
	void SetFloat    (const GLchar *name, GLfloat value);
	void SetInteger  (const GLchar *name, GLint value);
	void SetVector2f (const GLchar *name, GLfloat x, GLfloat y);
	void SetVector3f (const GLchar *name, GLfloat x, GLfloat y, GLfloat z);
	void SetVector4f (const GLchar *name, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
	void SetMatrix4  (const GLchar *name, const glm::mat4 &matrix);
};

/****************************************************************************************
* cOglGlyph
****************************************************************************************/
class cOglGlyph : public cListObject {
private:
	struct tKerning {
		public:
			tKerning(FT_ULong prevSym, GLfloat kerning = 0.0f)  {
				this->prevSym = prevSym;
				this->kerning = kerning;
			}
			FT_ULong prevSym;
			GLfloat kerning;
	};
	FT_ULong charCode;
	int bearingLeft;
	int bearingTop;
	int width;
	int height;
	int advanceX;
	cVector<tKerning> kerningCache;
	GLuint texture;
	void LoadTexture(FT_BitmapGlyph ftGlyph);
public:
	cOglGlyph(FT_ULong charCode, FT_BitmapGlyph ftGlyph);
	virtual ~cOglGlyph();
	FT_ULong CharCode(void) { return charCode; }
	int AdvanceX(void) { return advanceX; }
	int BearingLeft(void) const { return bearingLeft; }
	int BearingTop(void) const { return bearingTop; }
	int Width(void) const { return width; }
	int Height(void) const { return height; }
	int GetKerningCache(FT_ULong prevSym);
	void SetKerningCache(FT_ULong prevSym, int kerning);
	void BindTexture(void);
};

/****************************************************************************************
* cOglAtlasGlyph
****************************************************************************************/
class cOglAtlasGlyph : public cListObject {
private:
	struct tKerning {
		public:
			tKerning(FT_ULong prevSym, GLfloat kerning = 0.0f)  {
				this->prevSym = prevSym;
				this->kerning = kerning;
			}
			FT_ULong prevSym;
			GLfloat kerning;
	};
	FT_ULong charCode;
	int bearingLeft;
	int bearingTop;
	int width;
	int height;
	int advanceX;
	int advanceY;
	float xoffset;
	float yoffset;
	cVector<tKerning> kerningCache;
public:
	cOglAtlasGlyph(FT_ULong charCode, float advanceX, float advanceY, float width, float height, float bearingLeft, float bearingTop, float xoffset, float yoffset);
	virtual ~cOglAtlasGlyph();
	FT_ULong CharCode(void) { return charCode; }
	int AdvanceX(void) { return advanceX; }
	int AdvanceY(void) { return advanceY; }
	int BearingLeft(void) const { return bearingLeft; }
	int BearingTop(void) const { return bearingTop; }
	int Width(void) const { return width; }
	int Height(void) const { return height; }
	float XOffset(void) const { return xoffset; }
	float YOffset(void) const { return yoffset; }
	int GetKerningCache(FT_ULong prevSym);
	void SetKerningCache(FT_ULong prevSym, int kerning);
};

/****************************************************************************************
* cOglFontAtlas
****************************************************************************************/
#define MIN_CHARCODE 32
#define MAX_CHARCODE 255
class cOglFontAtlas {
private:
	GLuint tex;
	int w;
	int h;
	int fontheight;
	cOglAtlasGlyph* Glyph[MAX_CHARCODE - MIN_CHARCODE + 1];
public:
	cOglFontAtlas(FT_Face face, int height);
	virtual ~cOglFontAtlas(void);
	cOglAtlasGlyph* GetGlyph(int sym) const;
	int FontHeight(void) const { return fontheight; }
	int Height(void) const { return h; }
	int Width(void) const { return w; }
	void BindTexture(void);
};

/****************************************************************************************
* cOglFont
****************************************************************************************/
class cOglFont : public cListObject {
private:
	static bool initiated;
	cString name;
	int size;
	int height;
	int bottom;
	static FT_Library ftLib;
	FT_Face face;
	static cList<cOglFont> *fonts;
	mutable cList<cOglGlyph> glyphCache;
	cOglFont(const char *fontName, int charHeight);
	static void Init(void);
	cOglFontAtlas *atlas;
public:
	virtual ~cOglFont(void);
	static cOglFont *Get(const char *name, int charHeight);
	cOglFontAtlas *Atlas(void) { return atlas; };
	static void Cleanup(void);
	const char *Name(void) { return *name; };
	int Size(void) { return size; };
	int Bottom(void) {return bottom; };
	int Height(void) {return height; };
	cOglGlyph* Glyph(FT_ULong charCode) const;
	int Kerning(cOglGlyph *glyph, FT_ULong prevSym) const;
	int AtlasKerning(cOglAtlasGlyph *glyph, FT_ULong prevSym) const;
};

/****************************************************************************************
* cOglFb
* Framebuffer Object - OpenGL part of a Pixmap
****************************************************************************************/
class cOglFb {
protected:
	bool initiated;
	GLuint fb;
	GLuint texture;
	GLint width, height;
	GLint viewPortWidth, viewPortHeight;
	bool scrollable;
public:
	cOglFb(GLint width, GLint height, GLint viewPortWidth, GLint viewPortHeight);
	virtual ~cOglFb(void);
	bool Initiated(void) { return initiated; }
	virtual bool Init(void);
	void Bind(void);
	void BindRead(void);
	virtual void BindWrite(void);
	virtual void Unbind(void);
	bool BindTexture(void);
	void Blit(GLint destX1, GLint destY1, GLint destX2, GLint destY2);
	GLint Width(void) { return width; };
	GLint Height(void) { return height; };
	bool Scrollable(void) { return scrollable; };
	GLint ViewportWidth(void) { return viewPortWidth; };
	GLint ViewportHeight(void) { return viewPortHeight; };
};

/****************************************************************************************
* cOglOutputFb
* Output Framebuffer Object - holds texture which is our "output framebuffer"
****************************************************************************************/
class cOglOutputFb : public cOglFb {
private:
public:
	GLuint fb;
	GLuint texture;
	cOglOutputFb(GLint width, GLint height);
	virtual ~cOglOutputFb(void);
	virtual bool Init(void);
	virtual void BindWrite(void);
	virtual void Unbind(void);
};

/****************************************************************************************
* cOglVb
* Vertex Buffer - OpenGl Vertices for the different drawing commands
****************************************************************************************/
enum eVertexBufferType {
	vbRect,
	vbEllipse,
	vbSlope,
	vbTexture,
	vbTextureSwapBR,
	vbText,
	vbCount
};

class cOglVb {
private:
	eVertexBufferType type;
	eShaderType shader;
	GLuint vao;
	GLuint vbo;
	GLuint positionLoc;
	GLuint texCoordsLoc;
	int sizeVertex1;
	int sizeVertex2;
	int numVertices;
	GLuint drawMode;
public:
	cOglVb(int type);
	virtual ~cOglVb(void);
	bool Init(void);
	void Bind(void);
	void Unbind(void);
	void ActivateShader(void);
	void EnableBlending(void);
	void DisableBlending(void);
	void SetShaderColor(GLint color);
	void SetShaderBorderColor(GLint bcolor);
	void SetShaderTexture(GLint value);
	void SetShaderAlpha(GLint alpha);
	void SetShaderProjectionMatrix(GLint width, GLint height);
	void SetVertexSubData(GLfloat *vertices, int count = 0);
	void SetVertexData(GLfloat *vertices, int count = 0);
	void DrawArrays(int count = 0);
};

/****************************************************************************************
* cOpenGLCmd
****************************************************************************************/
class cOglCmd {
protected:
	cOglFb *fb;
public:
	cOglCmd(cOglFb *fb) { this->fb = fb; };
	virtual ~cOglCmd(void) {};
	virtual const char* Description(void) = 0;
	virtual bool Execute(void) = 0;
};

class cOglCmdInitOutputFb : public cOglCmd {
private:
	cOglOutputFb *oFb;
public:
	cOglCmdInitOutputFb(cOglOutputFb *oFb);
	virtual ~cOglCmdInitOutputFb(void) {};
	virtual const char* Description(void) { return "InitOutputFramebuffer"; }
	virtual bool Execute(void);
};

class cOglCmdInitFb : public cOglCmd {
private:
	cCondWait *wait;
public:
	cOglCmdInitFb(cOglFb *fb, cCondWait *wait = NULL);
	virtual ~cOglCmdInitFb(void) {};
	virtual const char* Description(void) { return "InitFramebuffer"; }
	virtual bool Execute(void);
};

class cOglCmdDeleteFb : public cOglCmd {
public:
	cOglCmdDeleteFb(cOglFb *fb);
	virtual ~cOglCmdDeleteFb(void) {};
	virtual const char* Description(void) { return "DeleteFramebuffer"; }
	virtual bool Execute(void);
};

class cOglCmdRenderFbToBufferFb : public cOglCmd {
private:
	cOglFb *buffer;
	GLfloat x, y;
	GLfloat drawPortX, drawPortY;
	GLint transparency;
	GLint bcolor;
	GLint dirtyX;
	GLint dirtyTop;
	GLint dirtyWidth;
	GLint dirtyHeight;
	bool alphablending;
	cSoftHdDevice *Device;
public:
	cOglCmdRenderFbToBufferFb(cOglFb *fb, cOglFb *buffer, GLint x, GLint y, GLint transparency, GLint drawPortX, GLint drawPortY, GLint dirtyX, GLint dirtyTop, GLint dirtyWidth, GLint dirtyHeight, bool alphablending, cSoftHdDevice *device);
	virtual ~cOglCmdRenderFbToBufferFb(void) {};
	virtual const char* Description(void) { return "Render Framebuffer to Buffer"; }
	virtual bool Execute(void);
};

class cOglCmdCopyBufferToOutputFb : public cOglCmd {
private:
	cOglOutputFb *oFb;
	GLfloat x, y;
	GLint bcolor;
	int active;
	cSoftHdDevice *Device;
public:
	cOglCmdCopyBufferToOutputFb(cOglFb *fb, cOglOutputFb *oFb, GLint x, GLint y, int active, cSoftHdDevice *device);
	virtual ~cOglCmdCopyBufferToOutputFb(void) {};
	virtual const char* Description(void) { return "Copy buffer to OutputFramebuffer"; }
	virtual bool Execute(void);
};

class cOglCmdFill : public cOglCmd {
private:
	GLint color;
public:
	cOglCmdFill(cOglFb *fb, GLint color);
	virtual ~cOglCmdFill(void) {};
	virtual const char* Description(void) { return "Fill"; }
	virtual bool Execute(void);
};

class cOglCmdBufferFill : public cOglCmd {
private:
	GLint color;
public:
	cOglCmdBufferFill(cOglFb *fb, GLint color);
	virtual ~cOglCmdBufferFill(void) {};
	virtual const char* Description(void) { return "Fill Buffer  "; }
	virtual bool Execute(void);
};

class cOglCmdDrawRectangle : public cOglCmd {
private:
	GLint x, y;
	GLint width, height;
	GLint color;
public:
	cOglCmdDrawRectangle(cOglFb *fb, GLint x, GLint y, GLint width, GLint height, GLint color);
	virtual ~cOglCmdDrawRectangle(void) {};
	virtual const char* Description(void) { return "DrawRectangle"; }
	virtual bool Execute(void);
};

class cOglCmdDrawEllipse : public cOglCmd {
private:
	GLint x, y;
	GLint width, height;
	GLint color;
	GLint quadrants;
	GLfloat *CreateVerticesFull(int &numVertices);
	GLfloat *CreateVerticesQuadrant(int &numVertices);
	GLfloat *CreateVerticesHalf(int &numVertices);
public:
	cOglCmdDrawEllipse(cOglFb *fb, GLint x, GLint y, GLint width, GLint height, GLint color, GLint quadrants);
	virtual ~cOglCmdDrawEllipse(void) {};
	virtual const char* Description(void) { return "DrawEllipse  "; }
	virtual bool Execute(void);
};

class cOglCmdDrawSlope : public cOglCmd {
private:
	GLint x, y;
	GLint width, height;
	GLint color;
	GLint type;
public:
	cOglCmdDrawSlope(cOglFb *fb, GLint x, GLint y, GLint width, GLint height, GLint color, GLint type);
	virtual ~cOglCmdDrawSlope(void) {};
	virtual const char* Description(void) { return "DrawSlope    "; }
	virtual bool Execute(void);
};

class cOglCmdDrawText : public cOglCmd {
private:
	GLint x, y;
	GLint limitX;
	GLint colorText;
	int length;
	cString fontName;
	int fontSize;
	unsigned int *symbols;
public:
	cOglCmdDrawText(cOglFb *fb, GLint x, GLint y, unsigned int *symbols, GLint limitX, const char *name, int fontSize, tColor colorText, int length);
	virtual ~cOglCmdDrawText(void);
	virtual const char* Description(void) { return "DrawText     "; }
	virtual bool Execute(void);
};

class cOglCmdDrawImage : public cOglCmd {
private:
	tColor *argb;
	GLint x, y, width, height;
	bool overlay;
	GLfloat scaleX, scaleY;
	GLint bcolor;
public:
	cOglCmdDrawImage(cOglFb *fb, tColor *argb, GLint width, GLint height, GLint x, GLint y, bool overlay = true, double scaleX = 1.0f, double scaleY = 1.0f);
	virtual ~cOglCmdDrawImage(void);
	virtual const char* Description(void) { return "Draw Image"; }
	virtual bool Execute(void);
};

class cOglCmdDrawTexture : public cOglCmd {
private:
	sOglImage *imageRef;
	GLint x, y;
	GLfloat scaleX, scaleY;
	GLint bcolor;
public:
	cOglCmdDrawTexture(cOglFb *fb, sOglImage *imageRef, GLint x, GLint y, double scaleX = 1.0f, double scaleY = 1.0f);
	virtual ~cOglCmdDrawTexture(void) {};
	virtual const char* Description(void) { return "Draw Texture"; }
	virtual bool Execute(void);
};

class cOglCmdStoreImage : public cOglCmd {
private:
	sOglImage *imageRef;
	tColor *data;
public:
	cOglCmdStoreImage(sOglImage *imageRef, tColor *argb);
	virtual ~cOglCmdStoreImage(void);
	virtual const char* Description(void) { return "Store Image"; }
	virtual bool Execute(void);
};

class cOglCmdDropImage : public cOglCmd {
private:
	sOglImage *imageRef;
	cCondWait *wait;
public:
	cOglCmdDropImage(sOglImage *imageRef, cCondWait *wait);
	virtual ~cOglCmdDropImage(void) {};
	virtual const char* Description(void) { return "Drop Image"; }
	virtual bool Execute(void);
};

/******************************************************************************
* cOglThread
******************************************************************************/
#define OGL_MAX_OSDIMAGES 512
#define OGL_CMDQUEUE_SIZE 200

class cOglThread : public cThread {
private:
	cCondWait *startWait;
	cCondWait *wait;
	bool stalled;
	std::queue<cOglCmd*> commands;
	GLint maxTextureSize;
	sOglImage imageCache[OGL_MAX_OSDIMAGES];
	long memCached;
	long maxCacheSize;
	bool InitOpenGL(void);
	bool InitShaders(void);
	void DeleteShaders(void);
	bool InitVertexBuffers(void);
	void DeleteVertexBuffers(void);
	void Cleanup(void);
	int GetFreeSlot(void);
	void ClearSlot(int slot);
	void eglAcquireContext(void);
	void eglReleaseContext(void);
	cVideoRender *Render;
protected:
	virtual void Action(void);
public:
	cOglThread(cCondWait *startWait, int maxCacheSize, cSoftHdDevice *device);
	virtual ~cOglThread();
	void Stop(void);
	void DoCmd(cOglCmd* cmd);
	int StoreImage(const cImage &image);
	void DropImageData(int imageHandle);
	sOglImage *GetImageRef(int slot);
	int MaxTextureSize(void) { return maxTextureSize; };
};

/****************************************************************************************
* cOglPixmap
****************************************************************************************/
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
	cOglFb *m_pFramebuffer;
	std::shared_ptr<cOglThread> m_pOglThread;
	bool m_dirty = true;
#ifdef GRIDPOINTS
	cFont *m_pTinyfont;

	void DrawGridRect(const cRect &, int, int, tColor, tColor, const cFont *);
	void DrawGridText(const cPoint &, const char *, tColor, tColor, const cFont *, int Width = 0, int Height = 0, int Alignment = taDefault);
#endif
	void DrawTextInternal(const cPoint &, const char *, tColor, tColor, const cFont *, int Width = 0, int Height = 0, int Alignment = taDefault, bool isGridText = false);
};

/******************************************************************************
* cOglOsd
******************************************************************************/
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

	static cOglOutputFb *OutputFramebuffer;        ///< main OSD output framebuffer - this keeps our finished "OSD"
private:
	cOglFb *m_pBufferFramebuffer = nullptr;        ///< pointer to framebuffer, where all pixmaps are blit in before the real flush
	std::shared_ptr<cOglThread> m_pOglThread;      ///< pointer to thread, which executes the commands
	cVector<cOglPixmap *> m_pOglPixmaps;           ///< pixmap array
	bool m_isSubtitleOsd;                          ///< is this a subtitle osd?
	cSize m_maxPixmapSize;                         ///< maximum allowed size of a pixmap
	cRect m_pDirtyViewport;                        ///< the dirty viewport
	cSoftHdDevice *m_pDevice;                      ///< pointer to cSofthdDevice
};

#endif
