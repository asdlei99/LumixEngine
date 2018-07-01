#include "ffr.h"
#include "engine/blob.h"
#include "engine/log.h"
#include "engine/math_utils.h"
#include <Windows.h>
#include <gl/GL.h>
#include "renderdoc_app.h"

#define FFR_GL_IMPORT(prototype, name) static prototype name;
#define FFR_GL_IMPORT_TYPEDEFS

#include "gl_ext.h"

#undef FFR_GL_IMPORT_TYPEDEFS
#undef FFR_GL_IMPORT

namespace Lumix
{


static struct {
	RENDERDOC_API_1_1_2* rdoc_api;
	GLuint vao;
	GLuint tex_buffers[32];
	IAllocator* allocator;
} s_ffr;


namespace DDS
{

static const uint DDS_MAGIC = 0x20534444; //  little-endian
static const uint DDSD_CAPS = 0x00000001;
static const uint DDSD_HEIGHT = 0x00000002;
static const uint DDSD_WIDTH = 0x00000004;
static const uint DDSD_PITCH = 0x00000008;
static const uint DDSD_PIXELFORMAT = 0x00001000;
static const uint DDSD_MIPMAPCOUNT = 0x00020000;
static const uint DDSD_LINEARSIZE = 0x00080000;
static const uint DDSD_DEPTH = 0x00800000;
static const uint DDPF_ALPHAPIXELS = 0x00000001;
static const uint DDPF_FOURCC = 0x00000004;
static const uint DDPF_INDEXED = 0x00000020;
static const uint DDPF_RGB = 0x00000040;
static const uint DDSCAPS_COMPLEX = 0x00000008;
static const uint DDSCAPS_TEXTURE = 0x00001000;
static const uint DDSCAPS_MIPMAP = 0x00400000;
static const uint DDSCAPS2_CUBEMAP = 0x00000200;
static const uint DDSCAPS2_CUBEMAP_POSITIVEX = 0x00000400;
static const uint DDSCAPS2_CUBEMAP_NEGATIVEX = 0x00000800;
static const uint DDSCAPS2_CUBEMAP_POSITIVEY = 0x00001000;
static const uint DDSCAPS2_CUBEMAP_NEGATIVEY = 0x00002000;
static const uint DDSCAPS2_CUBEMAP_POSITIVEZ = 0x00004000;
static const uint DDSCAPS2_CUBEMAP_NEGATIVEZ = 0x00008000;
static const uint DDSCAPS2_VOLUME = 0x00200000;
static const uint D3DFMT_DXT1 = '1TXD';
static const uint D3DFMT_DXT2 = '2TXD';
static const uint D3DFMT_DXT3 = '3TXD';
static const uint D3DFMT_DXT4 = '4TXD';
static const uint D3DFMT_DXT5 = '5TXD';

struct PixelFormat {
	uint dwSize;
	uint dwFlags;
	uint dwFourCC;
	uint dwRGBBitCount;
	uint dwRBitMask;
	uint dwGBitMask;
	uint dwBBitMask;
	uint dwAlphaBitMask;
};

struct Caps2 {
	uint dwCaps1;
	uint dwCaps2;
	uint dwDDSX;
	uint dwReserved;
};

struct Header {
	uint dwMagic;
	uint dwSize;
	uint dwFlags;
	uint dwHeight;
	uint dwWidth;
	uint dwPitchOrLinearSize;
	uint dwDepth;
	uint dwMipMapCount;
	uint dwReserved1[11];

	PixelFormat pixelFormat;
	Caps2 caps2;

	uint dwReserved2;
};

struct LoadInfo {
	bool compressed;
	bool swap;
	bool palette;
	uint divSize;
	uint blockBytes;
	GLenum internalFormat;
	GLenum externalFormat;
	GLenum type;
};

static uint sizeDXTC(uint w, uint h, GLuint format) {
    return ((w + 3) / 4) * ((h + 3) / 4) * (format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT ? 8 : 16);
}

static bool isDXT1(PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_FOURCC) && (pf.dwFourCC == D3DFMT_DXT1));
}

static bool isDXT3(PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_FOURCC) && (pf.dwFourCC == D3DFMT_DXT3));

}

static bool isDXT5(PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_FOURCC) && (pf.dwFourCC == D3DFMT_DXT5));
}

static bool isBGRA8(PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_RGB)
		&& (pf.dwFlags & DDPF_ALPHAPIXELS)
		&& (pf.dwRGBBitCount == 32)
		&& (pf.dwRBitMask == 0xff0000)
		&& (pf.dwGBitMask == 0xff00)
		&& (pf.dwBBitMask == 0xff)
		&& (pf.dwAlphaBitMask == 0xff000000U));
}

static bool isBGR8(PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_ALPHAPIXELS)
		&& !(pf.dwFlags & DDPF_ALPHAPIXELS)
		&& (pf.dwRGBBitCount == 24)
		&& (pf.dwRBitMask == 0xff0000)
		&& (pf.dwGBitMask == 0xff00)
		&& (pf.dwBBitMask == 0xff));
}

static bool isBGR5A1(PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_RGB)
		&& (pf.dwFlags & DDPF_ALPHAPIXELS)
		&& (pf.dwRGBBitCount == 16)
		&& (pf.dwRBitMask == 0x00007c00)
		&& (pf.dwGBitMask == 0x000003e0)
		&& (pf.dwBBitMask == 0x0000001f)
		&& (pf.dwAlphaBitMask == 0x00008000));
}

static bool isBGR565(PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_RGB)
		&& !(pf.dwFlags & DDPF_ALPHAPIXELS)
		&& (pf.dwRGBBitCount == 16)
		&& (pf.dwRBitMask == 0x0000f800)
		&& (pf.dwGBitMask == 0x000007e0)
		&& (pf.dwBBitMask == 0x0000001f));
}

static bool isINDEX8(PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_INDEXED) && (pf.dwRGBBitCount == 8));
}

static LoadInfo loadInfoDXT1 = {
	true, false, false, 4, 8, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
};
static LoadInfo loadInfoDXT3 = {
	true, false, false, 4, 16, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
};
static LoadInfo loadInfoDXT5 = {
	true, false, false, 4, 16, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
};
static LoadInfo loadInfoBGRA8 = {
	false, false, false, 1, 4, GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE
};
static LoadInfo loadInfoBGR8 = {
	false, false, false, 1, 3, GL_RGB8, GL_BGR, GL_UNSIGNED_BYTE
};
static LoadInfo loadInfoBGR5A1 = {
	false, true, false, 1, 2, GL_RGB5_A1, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV
};
static LoadInfo loadInfoBGR565 = {
	false, true, false, 1, 2, GL_RGB5, GL_RGB, GL_UNSIGNED_SHORT_5_6_5
};
static LoadInfo loadInfoIndex8 = {
	false, false, true, 1, 1, GL_RGB8, GL_BGRA, GL_UNSIGNED_BYTE
};

struct DXTColBlock
{
	uint16_t col0;
	uint16_t col1;
	u8 row[4];
};

struct DXT3AlphaBlock
{
	uint16_t row[4];
};

struct DXT5AlphaBlock
{
	u8 alpha0;
	u8 alpha1;
	u8 row[6];
};

static LUMIX_FORCE_INLINE void swapMemory(void* mem1, void* mem2, int size)
{
	if(size < 2048)
	{
		u8 tmp[2048];
		memcpy(tmp, mem1, size);
		memcpy(mem1, mem2, size);
		memcpy(mem2, tmp, size);
	}
	else
	{
		Array<u8> tmp(*s_ffr.allocator);
		tmp.resize(size);
		memcpy(&tmp[0], mem1, size);
		memcpy(mem1, mem2, size);
		memcpy(mem2, &tmp[0], size);
	}
}

static void flipBlockDXTC1(DXTColBlock *line, int numBlocks)
{
	DXTColBlock *curblock = line;

	for (int i = 0; i < numBlocks; i++)
	{
		swapMemory(&curblock->row[0], &curblock->row[3], sizeof(u8));
		swapMemory(&curblock->row[1], &curblock->row[2], sizeof(u8));
		++curblock;
	}
}

static void flipBlockDXTC3(DXTColBlock *line, int numBlocks)
{
	DXTColBlock *curblock = line;
	DXT3AlphaBlock *alphablock;

	for (int i = 0; i < numBlocks; i++)
	{
		alphablock = (DXT3AlphaBlock*)curblock;

		swapMemory(&alphablock->row[0], &alphablock->row[3], sizeof(uint16_t));
		swapMemory(&alphablock->row[1], &alphablock->row[2], sizeof(uint16_t));
		++curblock;

		swapMemory(&curblock->row[0], &curblock->row[3], sizeof(u8));
		swapMemory(&curblock->row[1], &curblock->row[2], sizeof(u8));
		++curblock;
	}
}

static void flipDXT5Alpha(DXT5AlphaBlock *block)
{
	u8 tmp_bits[4][4];

	const uint mask = 0x00000007;
	uint bits = 0;
	memcpy(&bits, &block->row[0], sizeof(u8) * 3);

	tmp_bits[0][0] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[0][1] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[0][2] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[0][3] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[1][0] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[1][1] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[1][2] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[1][3] = (u8)(bits & mask);

	bits = 0;
	memcpy(&bits, &block->row[3], sizeof(u8) * 3);

	tmp_bits[2][0] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[2][1] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[2][2] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[2][3] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[3][0] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[3][1] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[3][2] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[3][3] = (u8)(bits & mask);

	uint *out_bits = (uint*)&block->row[0];

	*out_bits = *out_bits | (tmp_bits[3][0] << 0);
	*out_bits = *out_bits | (tmp_bits[3][1] << 3);
	*out_bits = *out_bits | (tmp_bits[3][2] << 6);
	*out_bits = *out_bits | (tmp_bits[3][3] << 9);

	*out_bits = *out_bits | (tmp_bits[2][0] << 12);
	*out_bits = *out_bits | (tmp_bits[2][1] << 15);
	*out_bits = *out_bits | (tmp_bits[2][2] << 18);
	*out_bits = *out_bits | (tmp_bits[2][3] << 21);

	out_bits = (uint*)&block->row[3];

	*out_bits &= 0xff000000;

	*out_bits = *out_bits | (tmp_bits[1][0] << 0);
	*out_bits = *out_bits | (tmp_bits[1][1] << 3);
	*out_bits = *out_bits | (tmp_bits[1][2] << 6);
	*out_bits = *out_bits | (tmp_bits[1][3] << 9);

	*out_bits = *out_bits | (tmp_bits[0][0] << 12);
	*out_bits = *out_bits | (tmp_bits[0][1] << 15);
	*out_bits = *out_bits | (tmp_bits[0][2] << 18);
	*out_bits = *out_bits | (tmp_bits[0][3] << 21);
}

static void flipBlockDXTC5(DXTColBlock *line, int numBlocks)
{
	DXTColBlock *curblock = line;
	DXT5AlphaBlock *alphablock;

	for (int i = 0; i < numBlocks; i++)
	{
		alphablock = (DXT5AlphaBlock*)curblock;

		flipDXT5Alpha(alphablock);

		++curblock;

		swapMemory(&curblock->row[0], &curblock->row[3], sizeof(u8));
		swapMemory(&curblock->row[1], &curblock->row[2], sizeof(u8));

		++curblock;
	}
}

/// from gpu gems
static void flipCompressedTexture(int w, int h, int format, void* surface)
{
	void (*flipBlocksFunction)(DXTColBlock*, int);
	int xblocks = w >> 2;
	int yblocks = h >> 2;
	int blocksize;

	switch (format)
	{
		case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
			blocksize = 8;
			flipBlocksFunction = &flipBlockDXTC1;
			break;
		case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
			blocksize = 16;
			flipBlocksFunction = &flipBlockDXTC3;
			break;
		case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
			blocksize = 16;
			flipBlocksFunction = &flipBlockDXTC5;
			break;
		default:
			ASSERT(false);
			return;
	}

	int linesize = xblocks * blocksize;

	DXTColBlock *top = (DXTColBlock*)surface;
	DXTColBlock *bottom = (DXTColBlock*)((u8*)surface + ((yblocks - 1) * linesize));

	while (top < bottom)
	{
		(*flipBlocksFunction)(top, xblocks);
		(*flipBlocksFunction)(bottom, xblocks);
		swapMemory(bottom, top, linesize);

		top = (DXTColBlock*)((u8*)top + linesize);
		bottom = (DXTColBlock*)((u8*)bottom - linesize);
	}
}

} // namespace DDS

namespace ffr
{

#define CHECK_GL(gl) \
	do { \
		gl; \
		GLenum err = glGetError(); \
		if (err != GL_NO_ERROR) { \
			g_log_error.log("Renderer") << "OpenGL error " << err; \
		} \
	} while(0)



static void try_load_renderdoc()
{
	HMODULE lib = LoadLibrary("renderdoc.dll");
	if (!lib) return;
	pRENDERDOC_GetAPI RENDERDOC_GetAPI = (pRENDERDOC_GetAPI)GetProcAddress(lib, "RENDERDOC_GetAPI");
	if (RENDERDOC_GetAPI) {
		RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_1_2, (void **)&s_ffr.rdoc_api);
		s_ffr.rdoc_api->MaskOverlayBits(~RENDERDOC_OverlayBits::eRENDERDOC_Overlay_Enabled, 0);
	}
	/**/
	//FreeLibrary(lib);
}


static int load_gl()
{
	#define FFR_GL_IMPORT(prototype, name) \
		do { \
			name = (prototype)wglGetProcAddress(#name); \
			if (!name) { \
				g_log_error.log("Renderer") << "Failed to load GL function " #name "."; \
				return 0; \
			} \
		} while(0)

	#include "gl_ext.h"

	#undef FFR_GL_IMPORT

	return 1;
}


static int getSize(AttributeType type)
{
	switch(type) {
		case AttributeType::FLOAT: return 4;
		case AttributeType::U8: return 1;
		case AttributeType::I16: return 2;
		default: ASSERT(false); return 0;
	}
}


void VertexDecl::addAttribute(uint components_num, AttributeType type, bool normalized, bool as_int)
{
	if((int)attributes_count >= lengthOf(attributes)) {
		ASSERT(false);
		return;
	}

	Attribute& attr = attributes[attributes_count];
	attr.components_num = components_num;
	attr.as_int = as_int;
	attr.normalized = normalized;
	attr.type = type;
	attr.offset = 0;
	if(attributes_count > 0) {
		const Attribute& prev = attributes[attributes_count - 1];
		attr.offset = prev.offset + prev.components_num * getSize(prev.type);
	}
	size = attr.offset + attr.components_num * getSize(attr.type);
	++attributes_count;
}


void viewport(uint x,uint y,uint w,uint h)
{
	glViewport(x, y, w, h);
}


void blending(int mode)
{
	if (mode) {
		glEnable(GL_BLEND);
	}
	else {
		glDisable(GL_BLEND);
	}
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}


void scissor(uint x,uint y,uint w,uint h)
{
	glScissor(x, y, w, h);
}


void draw(const DrawCall& dc)
{
	if (!dc.shader.isValid()) return;

	if( dc.state & u64(StateFlags::DEPTH_TEST)) glEnable(GL_DEPTH_TEST);
	else glDisable(GL_DEPTH_TEST);

	if(dc.state & u64(StateFlags::CULL_BACK)) {
		glEnable(GL_CULL_FACE);
		glCullFace(GL_BACK);
	}
	if(dc.state & u64(StateFlags::CULL_FRONT)) {
		glEnable(GL_CULL_FACE);
		glCullFace(GL_FRONT); 
	}
	else {
		glDisable(GL_CULL_FACE);
	}

	glPolygonMode(GL_FRONT_AND_BACK, dc.state & u64(StateFlags::WIREFRAME) ? GL_LINE : GL_FILL);

	const GLuint prg = dc.shader.value;
	CHECK_GL(glUseProgram(prg));

	GLuint pt;
	switch (dc.primitive_type) {
		case PrimitiveType::TRIANGLES: pt = GL_TRIANGLES; break;
		case PrimitiveType::TRIANGLE_STRIP: pt = GL_TRIANGLE_STRIP; break;
		case PrimitiveType::LINES: pt = GL_LINES; break;
		default: ASSERT(0); break;
	}

	for (uint i = 0; i < dc.tex_buffers_count; ++i) {
		const GLuint buf = dc.tex_buffers[i].value;
		CHECK_GL(glActiveTexture(GL_TEXTURE0 + i));
		CHECK_GL(glBindTexture(GL_TEXTURE_BUFFER, s_ffr.tex_buffers[i]));
		CHECK_GL(glTexBuffer(GL_TEXTURE_BUFFER, GL_R32F, buf));
		const GLint uniform_loc = glGetUniformLocation(prg, "test");
		CHECK_GL(glUniform1i(uniform_loc, i));
	}

	for (uint i = 0; i < dc.textures_count; ++i) {
		const GLuint t = dc.textures[i].value;
		CHECK_GL(glActiveTexture(GL_TEXTURE0 + i));
		glBindTexture(GL_TEXTURE_2D, t);
		const GLint uniform_loc = glGetUniformLocation(prg, "test");
		CHECK_GL(glUniform1i(uniform_loc, i));
	}

	if (dc.vertex_decl) {
		const VertexDecl* decl = dc.vertex_decl;
		const GLsizei stride = decl->size;
		const GLuint vb = dc.vertex_buffer.value;
		const uint vb_offset = dc.vertex_buffer_offset;
		glBindBuffer(GL_ARRAY_BUFFER, vb);

		for (uint i = 0; i < decl->attributes_count; ++i) {
			const Attribute* attr = &decl->attributes[i];
			const void* offset = (void*)(intptr_t)(attr->offset + vb_offset);
			GLenum gl_attr_type;
			switch (attr->type) {
				case AttributeType::I16: gl_attr_type = GL_SHORT; break;
				case AttributeType::FLOAT: gl_attr_type = GL_FLOAT; break;
				case AttributeType::U8: gl_attr_type = GL_UNSIGNED_BYTE; break;
			}
			const int index = dc.attribute_map ? dc.attribute_map[i] : i;

			if(index >= 0) {
				glEnableVertexAttribArray(index);
				glVertexAttribPointer(index, attr->components_num, gl_attr_type, attr->normalized, stride, offset);
			}
		}
	}

	if (dc.index_buffer.value != 0xffFFffFF) {
		const GLuint ib = dc.index_buffer.value;
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib);
		glDrawElements(pt, dc.indices_count, GL_UNSIGNED_SHORT, (void*)(intptr_t)(dc.indices_offset * sizeof(short)));
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	}
	else {
		CHECK_GL(glDrawArrays(pt, dc.indices_offset, dc.indices_count));
	}
}


void uniformBlockBinding(ProgramHandle program, const char* block_name, uint binding)
{
	const GLint index = glGetUniformBlockIndex(program.value, block_name);
	glUniformBlockBinding(program.value, index, binding);
}


void bindUniformBuffer(uint index, BufferHandle buffer, size_t offset, size_t size)
{
	glBindBufferRange(GL_UNIFORM_BUFFER, index, buffer.value, offset, size);
}


void update(BufferHandle buffer, const void* data, size_t offset, size_t size)
{
	const GLuint buf = buffer.value;
	CHECK_GL(glBindBuffer(GL_UNIFORM_BUFFER, buf));
	CHECK_GL(glBufferSubData(GL_UNIFORM_BUFFER, offset, size, data));
	CHECK_GL(glBindBuffer(GL_UNIFORM_BUFFER, 0));
}


BufferHandle createBuffer(size_t size, const void* data)
{
	GLuint buf;
	CHECK_GL(glGenBuffers(1, &buf));
	CHECK_GL(glBindBuffer(GL_UNIFORM_BUFFER, buf));
	CHECK_GL(glBufferData(GL_UNIFORM_BUFFER, size, data, GL_STATIC_DRAW));
	CHECK_GL(glBindBuffer(GL_UNIFORM_BUFFER, 0));

	return { buf };
}


void destroy(ProgramHandle program)
{
	if (program.isValid()) {
		glDeleteProgram(program.value);
	}
}

static struct {
	TextureFormat format;
	GLenum gl_internal;
	GLenum gl_format;
	GLenum type;
} s_texture_formats[] =
{ 
	{TextureFormat::D24, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT},
	{TextureFormat::D24S8, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8},
	{TextureFormat::D32, GL_DEPTH_COMPONENT32, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT},
	
	{TextureFormat::RGBA8, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE},
	{TextureFormat::RGBA16F, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT},
	{TextureFormat::R16F, GL_R16F, GL_RED, GL_HALF_FLOAT},
	{TextureFormat::R16, GL_R16, GL_RED, GL_UNSIGNED_SHORT},
	{TextureFormat::R32F, GL_R32F, GL_RED, GL_FLOAT}
};


TextureHandle loadTexture(const void* input, int input_size, TextureInfo* info)
{
	DDS::Header hdr;
	uint width = 0;
	uint height = 0;
	uint mipMapCount = 0;

	InputBlob blob(input, input_size);
	blob.read(&hdr, sizeof(hdr));

	if (hdr.dwMagic != DDS::DDS_MAGIC || hdr.dwSize != 124 ||
		!(hdr.dwFlags & DDS::DDSD_PIXELFORMAT) || !(hdr.dwFlags & DDS::DDSD_CAPS))
	{
		g_log_error.log("renderer") << "Wrong dds format or corrupted dds.";
		return INVALID_TEXTURE;
	}

	width = hdr.dwWidth;
	height = hdr.dwHeight;

	DDS::LoadInfo* li;

	if (isDXT1(hdr.pixelFormat)) {
		li = &DDS::loadInfoDXT1;
	}
	else if (isDXT3(hdr.pixelFormat)) {
		li = &DDS::loadInfoDXT3;
	}
	else if (isDXT5(hdr.pixelFormat)) {
		li = &DDS::loadInfoDXT5;
	}
	else if (isBGRA8(hdr.pixelFormat)) {
		li = &DDS::loadInfoBGRA8;
	}
	else if (isBGR8(hdr.pixelFormat)) {
		li = &DDS::loadInfoBGR8;
	}
	else if (isBGR5A1(hdr.pixelFormat)) {
		li = &DDS::loadInfoBGR5A1;
	}
	else if (isBGR565(hdr.pixelFormat)) {
		li = &DDS::loadInfoBGR565;
	}
	else if (isINDEX8(hdr.pixelFormat)) {
		li = &DDS::loadInfoIndex8;
	}
	else {
		return INVALID_TEXTURE;
	}

	GLuint texture;
	glGenTextures(1, &texture);
	if (texture == 0) {
		return INVALID_TEXTURE;
	}

	glBindTexture(GL_TEXTURE_2D, texture);

	mipMapCount = (hdr.dwFlags & DDS::DDSD_MIPMAPCOUNT) ? hdr.dwMipMapCount : 1;
	if (li->compressed) {
		uint size = DDS::sizeDXTC(width, height, li->internalFormat);
		if (size != hdr.dwPitchOrLinearSize || (hdr.dwFlags & DDS::DDSD_LINEARSIZE) == 0) {
			glDeleteTextures(1, &texture);
			return INVALID_TEXTURE;
		}
		Array<u8> data(*s_ffr.allocator);
		data.resize(size);
		for (uint ix = 0; ix < mipMapCount; ++ix) {
			blob.read(&data[0], size);
			//DDS::flipCompressedTexture(width, height, li->internalFormat, &data[0]);
			glCompressedTexImage2D(GL_TEXTURE_2D, ix, li->internalFormat, width, height, 0, size, &data[0]);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			width = Math::maximum(1, width >> 1);
			height = Math::maximum(1, height >> 1);
			size = DDS::sizeDXTC(width, height, li->internalFormat);
		}
	}
	else if (li->palette) {
		if ((hdr.dwFlags & DDS::DDSD_PITCH) == 0 || hdr.pixelFormat.dwRGBBitCount != 8) {
			glDeleteTextures(1, &texture);
			return INVALID_TEXTURE;
		}
		uint size = hdr.dwPitchOrLinearSize * height;
		if (size != width * height * li->blockBytes) {
			glDeleteTextures(1, &texture);
			return INVALID_TEXTURE;
		}
		Array<u8> data(*s_ffr.allocator);
		data.resize(size);
		uint palette[256];
		Array<uint> unpacked(*s_ffr.allocator);
		unpacked.resize(size);
		blob.read(palette, 4 * 256);
		for (uint ix = 0; ix < mipMapCount; ++ix) {
			blob.read(&data[0], size);
			for (uint zz = 0; zz < size; ++zz) {
				unpacked[zz] = palette[data[zz]];
			}
			//glPixelStorei(GL_UNPACK_ROW_LENGTH, height);
			glTexImage2D(GL_TEXTURE_2D, ix, li->internalFormat, width, height, 0, li->externalFormat, li->type, &unpacked[0]);
			width = Math::maximum(1, width >> 1);
			height = Math::maximum(1, height >> 1);
			size = width * height * li->blockBytes;
		}
	}
	else {
		if (li->swap) {
			glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_TRUE);
		}
		uint size = width * height * li->blockBytes;
		Array<u8> data(*s_ffr.allocator);
		data.resize(size);
		for (uint ix = 0; ix < mipMapCount; ++ix) {
			blob.read(&data[0], size);
			//glPixelStorei(GL_UNPACK_ROW_LENGTH, height);
			glTexImage2D(GL_TEXTURE_2D, ix, li->internalFormat, width, height, 0, li->externalFormat, li->type, &data[0]);
			width = Math::maximum(1, width >> 1);
			height = Math::maximum(1, height >> 1);
			size = width * height * li->blockBytes;
		}
		glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, mipMapCount - 1);

	if(info) {
		info->width = width;
		info->height = height;
		info->depth = 1;
		info->layers = 1;
		info->mips = mipMapCount;
		info->is_cubemap = false;
	}

	return {texture};
}


TextureHandle createTexture(uint w,uint h, TextureFormat format, const void* data)
{
	GLuint t;
	CHECK_GL(glGenTextures(1, &t));
	CHECK_GL(glBindTexture(GL_TEXTURE_2D, t));
	int found_format = 0;
	for (int i = 0; i < sizeof(s_texture_formats) / sizeof(s_texture_formats[0]); ++i) {
		if(s_texture_formats[i].format == format) {
			CHECK_GL(glTexImage2D(GL_TEXTURE_2D
				, 0
				, s_texture_formats[i].gl_internal
				, w
				, h
				, 0
				, s_texture_formats[i].gl_format
				, s_texture_formats[i].type
				, data));
			found_format = 1;
			break;
		}
	}

	if(!found_format) {
		CHECK_GL(glBindTexture(GL_TEXTURE_2D, 0));
		CHECK_GL(glDeleteTextures(1, &t));
		return INVALID_TEXTURE;	
	}

	CHECK_GL(glGenerateMipmap(GL_TEXTURE_2D));
	CHECK_GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT));
	CHECK_GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT));
	CHECK_GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
	CHECK_GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR));

	CHECK_GL(glBindTexture(GL_TEXTURE_2D, 0));
	return {t};
}


void destroy(TextureHandle texture)
{
	CHECK_GL(glDeleteTextures(1, &texture.value));
}


void destroy(BufferHandle buffer)
{
	CHECK_GL(glDeleteBuffers(1, &buffer.value));
}


void clear(uint flags, const float* color, float depth)
{
	GLbitfield gl_flags = 0;
	if (flags & (uint)ClearFlags::COLOR) {
		CHECK_GL(glClearColor(color[0], color[1], color[2], color[3]));
		gl_flags |= GL_COLOR_BUFFER_BIT;
	}
	if (flags & (uint)ClearFlags::DEPTH) {
		CHECK_GL(glClearDepth(depth));
		gl_flags |= GL_DEPTH_BUFFER_BIT;
	}
	CHECK_GL(glUseProgram(0));
	CHECK_GL(glClear(gl_flags));
}

static const char* shaderTypeToString(ShaderType type)
{
	switch(type) {
		case ShaderType::FRAGMENT: return "fragment shader";
		case ShaderType::VERTEX: return "vertex shader";
		default: return "unknown shader type";
	}
}


ProgramHandle createProgram(const char** srcs, const ShaderType* types, int num, const char** prefixes, int prefixes_count, const char* name)
{
	const char* combined_srcs[16];
	ASSERT(prefixes_count < lengthOf(combined_srcs) - 1); 
	enum { MAX_SHADERS_PER_PROGRAM = 16 };

	if (num > MAX_SHADERS_PER_PROGRAM) {
		g_log_error.log("Renderer") << "Too many shaders per program in " << name;
		return INVALID_PROGRAM;
	}

	const GLuint prg = glCreateProgram();

	for (int i = 0; i < num; ++i) {
		GLenum shader_type;
		switch (types[i]) {
			case ShaderType::FRAGMENT: shader_type = GL_FRAGMENT_SHADER; break;
			case ShaderType::VERTEX: shader_type = GL_VERTEX_SHADER; break;
			default: ASSERT(0); break;
		}
		const GLuint shd = glCreateShader(shader_type);
		combined_srcs[prefixes_count] = srcs[i];
		for (int j = 0; j < prefixes_count; ++j) {
			combined_srcs[j] = prefixes[j];
		}

		CHECK_GL(glShaderSource(shd, 1 + prefixes_count, combined_srcs, 0));
		CHECK_GL(glCompileShader(shd));

		GLint compile_status;
		CHECK_GL(glGetShaderiv(shd, GL_COMPILE_STATUS, &compile_status));
		if (compile_status == GL_FALSE) {
			GLint log_len = 0;
			CHECK_GL(glGetShaderiv(shd, GL_INFO_LOG_LENGTH, &log_len));
			if (log_len > 0) {
				Array<char> log_buf(*s_ffr.allocator);
				log_buf.resize(log_len);
				CHECK_GL(glGetShaderInfoLog(shd, log_len, &log_len, &log_buf[0]));
				g_log_error.log("Renderer") << name << " - " << shaderTypeToString(types[i]) << ": " << &log_buf[0];
			}
			else {
				g_log_error.log("Renderer") << "Failed to compile shader " << name << " - " << shaderTypeToString(types[i]);
			}
			CHECK_GL(glDeleteShader(shd));
			return INVALID_PROGRAM;
		}

		CHECK_GL(glAttachShader(prg, shd));
		CHECK_GL(glDeleteShader(shd));
	}

	CHECK_GL(glLinkProgram(prg));
	GLint linked;
	CHECK_GL(glGetProgramiv(prg, GL_LINK_STATUS, &linked));

	if (linked == GL_FALSE) {
		GLint log_len = 0;
		CHECK_GL(glGetProgramiv(prg, GL_INFO_LOG_LENGTH, &log_len));
		if (log_len > 0) {
			Array<char> log_buf(*s_ffr.allocator);
			log_buf.resize(log_len);
			CHECK_GL(glGetProgramInfoLog(prg, log_len, &log_len, &log_buf[0]));
			g_log_error.log("Renderer") << name << ": " << &log_buf[0];
		}
		else {
			g_log_error.log("Renderer") << "Failed to link program " << name;
		}
		CHECK_GL(glDeleteProgram(prg));
		return INVALID_PROGRAM;
	}

	return { prg };
}


static void gl_debug_callback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const char *message, const void *userParam)
{
	if (type != GL_DEBUG_TYPE_ERROR && type != GL_DEBUG_TYPE_PERFORMANCE) return;
	g_log_error.log("GL") << message;
}


void preinit()
{
	try_load_renderdoc();
}


bool init(IAllocator& allocator)
{
	s_ffr.allocator = &allocator;
	
	if (!load_gl()) return false;

/*	int extensions_count;
	glGetIntegerv(GL_NUM_EXTENSIONS, &extensions_count);
	for(int i = 0; i < extensions_count; ++i) {
		const char* ext = (const char*)glGetStringi(GL_EXTENSIONS, i);
		OutputDebugString(ext);
		OutputDebugString("\n");
	}
	const unsigned char* extensions = glGetString(GL_EXTENSIONS);
	const unsigned char* version = glGetString(GL_VERSION);*/

	CHECK_GL(glEnable(GL_DEBUG_OUTPUT));
	CHECK_GL(glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS));
	CHECK_GL(glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, 0, GL_TRUE));
	CHECK_GL(glDebugMessageCallback(gl_debug_callback, 0));
	
	CHECK_GL(glGenVertexArrays(1, &s_ffr.vao));
	CHECK_GL(glBindVertexArray(s_ffr.vao));
	CHECK_GL(glGenTextures(_countof(s_ffr.tex_buffers), s_ffr.tex_buffers));

	return true;
}


void popDebugGroup()
{
	glPopDebugGroup();
}


void pushDebugGroup(const char* msg)
{
	glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, msg);
}


void destroy(FramebufferHandle fb)
{
	CHECK_GL(glDeleteFramebuffers(1, &fb.value));
}


int getAttribLocation(ProgramHandle program, const char* uniform_name)
{
	return glGetAttribLocation(program.value, uniform_name);
}


void setUniform1i(ProgramHandle program, const char* uniform_name, int value)
{
	CHECK_GL(glUseProgram(program.value));
	const GLint uniform_loc = glGetUniformLocation(program.value, uniform_name);
	CHECK_GL(glUniform1i(uniform_loc, value));
}


void setUniform2f(ProgramHandle program, const char* uniform_name, uint count, const float* value)
{
	CHECK_GL(glUseProgram(program.value));
	const GLint uniform_loc = glGetUniformLocation(program.value, uniform_name);
	CHECK_GL(glUniform2fv(uniform_loc, count, value));
}


void setUniform4f(ProgramHandle program, const char* uniform_name, uint count, const float* value)
{
	CHECK_GL(glUseProgram(program.value));
	const GLint uniform_loc = glGetUniformLocation(program.value, uniform_name);
	CHECK_GL(glUniform4fv(uniform_loc, count, value));
}


void setUniformMatrix4f(ProgramHandle program, const char* uniform_name, uint count, const float* value)
{
	CHECK_GL(glUseProgram(program.value));
	const GLint uniform_loc = glGetUniformLocation(program.value, uniform_name);
	CHECK_GL(glUniformMatrix4fv(uniform_loc, count, false, value));
}


FramebufferHandle createFramebuffer(uint renderbuffers_count, const TextureHandle* renderbuffers)
{
	GLuint fb;
	CHECK_GL(glGenFramebuffers(1, &fb));
	CHECK_GL(glBindFramebuffer(GL_FRAMEBUFFER, fb));
	
	int color_attachment_idx = 0;
	for (uint i = 0; i < renderbuffers_count; ++i) {
		const GLuint t = renderbuffers[i].value;
		CHECK_GL(glBindTexture(GL_TEXTURE_2D, t));
		GLint internal_format;
		CHECK_GL(glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal_format));

		CHECK_GL(glBindTexture(GL_TEXTURE_2D, 0));
		switch(internal_format) {
			case GL_DEPTH24_STENCIL8:
			case GL_DEPTH_COMPONENT24:
			case GL_DEPTH_COMPONENT32:
				CHECK_GL(glNamedFramebufferTexture(fb, GL_DEPTH_ATTACHMENT, t, 0));
				break;
			default:
				CHECK_GL(glNamedFramebufferTexture(fb, GL_COLOR_ATTACHMENT0 + color_attachment_idx, t, 0));
				++color_attachment_idx;
				break;
		}
	}

	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	CHECK_GL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		CHECK_GL(glDeleteFramebuffers(1, &fb));
		return INVALID_FRAMEBUFFER;
	}

	return {fb};
}


void setFramebuffer(FramebufferHandle fb)
{
	if(fb.value == 0xffFFffFF) {
		CHECK_GL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
	}
	else {
		CHECK_GL(glBindFramebuffer(GL_FRAMEBUFFER, fb.value));
	}
}


void shutdown()
{
}

} // ns ffr 

} // ns Lumix