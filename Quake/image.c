/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// image.c -- image loading

#include "quakedef.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4505)
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_LINEAR
#define STBI_MALLOC(sz)	     Mem_Alloc (sz)
#define STBI_REALLOC(p, newsz) Mem_Realloc (p, newsz)
#define STBI_FREE(p)	     Mem_Free (p)
#include "stb_image.h"

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"

static THREAD_LOCAL char loadfilename[MAX_OSPATH]; // file scope so that error messages can use it

typedef struct stdio_buffer_s
{
	FILE         *f;
	unsigned char buffer[1024];
	int           size;
	int           pos;
} stdio_buffer_t;

static stdio_buffer_t *Buf_Alloc (FILE *f)
{
	stdio_buffer_t *buf = (stdio_buffer_t *)Mem_Alloc (sizeof (stdio_buffer_t));
	buf->f = f;
	return buf;
}

static void Buf_Free (stdio_buffer_t *buf)
{
	Mem_Free (buf);
}

static inline int Buf_GetC (stdio_buffer_t *buf)
{
	if (buf->pos >= buf->size)
	{
		buf->size = fread (buf->buffer, 1, sizeof (buf->buffer), buf->f);
		buf->pos = 0;

		if (buf->size == 0)
			return EOF;
	}

	return buf->buffer[buf->pos++];
}

byte *Image_LoadImage (const char *name, int *width, int *height)
{
	FILE *f;

	q_snprintf (loadfilename, sizeof (loadfilename), "%s.tga", name);
	COM_FOpenFile (loadfilename, &f, NULL);
	if (f)
		return Image_LoadTGA (f, width, height, name);

	q_snprintf (loadfilename, sizeof (loadfilename), "%s.pcx", name);
	COM_FOpenFile (loadfilename, &f, NULL);
	if (f)
		return Image_LoadPCX (f, width, height);

	q_snprintf (loadfilename, sizeof (loadfilename), "%s.jpg", name);
	COM_FOpenFile (loadfilename, &f, NULL);
	if (f)
	{
		byte *data = stbi_load_from_file (f, width, height, NULL, 4);
		fclose (f);
		return data;
	}

	q_snprintf (loadfilename, sizeof (loadfilename), "%s.jpeg", name);
	COM_FOpenFile (loadfilename, &f, NULL);
	if (f)
	{
		byte *data = stbi_load_from_file (f, width, height, NULL, 4);
		fclose (f);
		return data;
	}

	q_snprintf (loadfilename, sizeof (loadfilename), "%s.png", name);
	COM_FOpenFile (loadfilename, &f, NULL);
	if (f)
	{
		byte *data = stbi_load_from_file (f, width, height, NULL, 4);
		fclose (f);
		return data;
	}

	return NULL;
}

byte *Image_LoadImageOSPath (const char *path, int *width, int *height)
{
	FILE *f = fopen (path, "rb");
	byte *data;

	if (f == NULL)
		return NULL;

	data = stbi_load_from_file (f, width, height, NULL, 4);
	fclose (f);
	return data;
}

//==============================================================================
//
//  TGA
//
//==============================================================================

typedef struct targaheader_s
{
	unsigned char  id_length, colormap_type, image_type;
	unsigned short colormap_index, colormap_length;
	unsigned char  colormap_size;
	unsigned short x_origin, y_origin, width, height;
	unsigned char  pixel_size, attributes;
} targaheader_t;

#define TARGAHEADERSIZE 18 // size on disk

int fgetLittleShort (FILE *f)
{
	byte b1, b2;

	b1 = fgetc (f);
	b2 = fgetc (f);

	return (short)(b1 + b2 * 256);
}

int fgetLittleLong (FILE *f)
{
	byte b1, b2, b3, b4;

	b1 = fgetc (f);
	b2 = fgetc (f);
	b3 = fgetc (f);
	b4 = fgetc (f);

	return b1 + (b2 << 8) + (b3 << 16) + (b4 << 24);
}

/*
============
Image_WriteTGA -- writes RGB or RGBA data to a TGA file

returns true if successful
============
*/
qboolean Image_WriteTGA (const char *name, byte *data, int width, int height, int bpp, qboolean upsidedown)
{
	int  handle, i, size, temp, bytes;
	char pathname[MAX_OSPATH];
	byte header[TARGAHEADERSIZE];

	Sys_mkdir (com_gamedir); // if we've switched to a nonexistant gamedir, create it now so we don't crash
	q_snprintf (pathname, sizeof (pathname), "%s/%s", com_gamedir, name);
	handle = Sys_FileOpenWrite (pathname);
	if (handle == -1)
		return false;

	memset (header, 0, TARGAHEADERSIZE);
	header[2] = 2; // uncompressed type
	header[12] = width & 255;
	header[13] = width >> 8;
	header[14] = height & 255;
	header[15] = height >> 8;
	header[16] = bpp; // pixel size
	if (upsidedown)
		header[17] = 0x20; // upside-down attribute

	// swap red and blue bytes
	bytes = bpp / 8;
	size = width * height * bytes;
	for (i = 0; i < size; i += bytes)
	{
		temp = data[i];
		data[i] = data[i + 2];
		data[i + 2] = temp;
	}

	Sys_FileWrite (handle, header, TARGAHEADERSIZE);
	Sys_FileWrite (handle, data, size);
	Sys_FileClose (handle);

	return true;
}

/*
=============
Image_LoadTGA
=============
*/
byte *Image_LoadTGA (FILE *fin, int *width, int *height, const char *name)
{
	targaheader_t   targa_header;
	int             columns, rows, numPixels;
	byte           *pixbuf;
	int             row, column;
	byte           *targa_rgba;
	int             realrow;     // johnfitz -- fix for upside-down targas
	qboolean        upside_down; // johnfitz -- fix for upside-down targas
	stdio_buffer_t *buf;

	targa_header.id_length = fgetc (fin);
	targa_header.colormap_type = fgetc (fin);
	targa_header.image_type = fgetc (fin);

	targa_header.colormap_index = fgetLittleShort (fin);
	targa_header.colormap_length = fgetLittleShort (fin);
	targa_header.colormap_size = fgetc (fin);
	targa_header.x_origin = fgetLittleShort (fin);
	targa_header.y_origin = fgetLittleShort (fin);
	targa_header.width = fgetLittleShort (fin);
	targa_header.height = fgetLittleShort (fin);
	targa_header.pixel_size = fgetc (fin);
	targa_header.attributes = fgetc (fin);

	if (targa_header.image_type == 1)
	{
		Con_Warning ("paletted TGA (less compatible): %s\n", name);
		if (targa_header.pixel_size != 8 || targa_header.colormap_size != 24 || targa_header.colormap_length > 256)
			Sys_Error ("Image_LoadTGA: %s has an %ibit palette", loadfilename, targa_header.colormap_type);
	}
	else
	{
		if (targa_header.image_type != 2 && targa_header.image_type != 10)
			Sys_Error ("Image_LoadTGA: %s is not a type 2 or type 10 targa (%i)", loadfilename, targa_header.image_type);

		if (targa_header.colormap_type != 0 || (targa_header.pixel_size != 32 && targa_header.pixel_size != 24))
			Sys_Error ("Image_LoadTGA: %s is not a 24bit or 32bit targa", loadfilename);
	}

	columns = targa_header.width;
	rows = targa_header.height;
	numPixels = columns * rows;
	upside_down = !(targa_header.attributes & 0x20); // johnfitz -- fix for upside-down targas

	targa_rgba = (byte *)Mem_Alloc (numPixels * 4);

	if (targa_header.id_length != 0)
		fseek (fin, targa_header.id_length, SEEK_CUR); // skip TARGA image comment

	buf = Buf_Alloc (fin);

	if (targa_header.image_type == 1) // Uncompressed, paletted images
	{
		byte palette[256 * 4];
		int  i;
		// palette data comes first
		for (i = 0; i < targa_header.colormap_length; i++)
		{ // this palette data is bgr.
			palette[i * 3 + 2] = Buf_GetC (buf);
			palette[i * 3 + 1] = Buf_GetC (buf);
			palette[i * 3 + 0] = Buf_GetC (buf);
			palette[i * 3 + 3] = 255;
		}
		for (i = targa_header.colormap_length * 4; i < sizeof (palette); i++)
			palette[i] = 0;
		for (row = rows - 1; row >= 0; row--)
		{
			realrow = upside_down ? row : rows - 1 - row;
			pixbuf = targa_rgba + realrow * columns * 4;

			for (column = 0; column < columns; column++)
			{
				i = Buf_GetC (buf);
				*pixbuf++ = palette[i * 3 + 0];
				*pixbuf++ = palette[i * 3 + 1];
				*pixbuf++ = palette[i * 3 + 2];
				*pixbuf++ = palette[i * 3 + 3];
			}
		}
	}
	else if (targa_header.image_type == 2) // Uncompressed, RGB images
	{
		for (row = rows - 1; row >= 0; row--)
		{
			// johnfitz -- fix for upside-down targas
			realrow = upside_down ? row : rows - 1 - row;
			pixbuf = targa_rgba + realrow * columns * 4;
			// johnfitz
			for (column = 0; column < columns; column++)
			{
				unsigned char red, green, blue, alphabyte;
				switch (targa_header.pixel_size)
				{
				case 24:
					blue = Buf_GetC (buf);
					green = Buf_GetC (buf);
					red = Buf_GetC (buf);
					*pixbuf++ = red;
					*pixbuf++ = green;
					*pixbuf++ = blue;
					*pixbuf++ = 255;
					break;
				case 32:
					blue = Buf_GetC (buf);
					green = Buf_GetC (buf);
					red = Buf_GetC (buf);
					alphabyte = Buf_GetC (buf);
					*pixbuf++ = red;
					*pixbuf++ = green;
					*pixbuf++ = blue;
					*pixbuf++ = alphabyte;
					break;
				}
			}
		}
	}
	else if (targa_header.image_type == 10) // Runlength encoded RGB images
	{
		unsigned char red, green, blue, alphabyte, packetHeader, packetSize, j;
		for (row = rows - 1; row >= 0; row--)
		{
			// johnfitz -- fix for upside-down targas
			realrow = upside_down ? row : rows - 1 - row;
			pixbuf = targa_rgba + realrow * columns * 4;
			// johnfitz
			for (column = 0; column < columns;)
			{
				packetHeader = Buf_GetC (buf);
				packetSize = 1 + (packetHeader & 0x7f);
				if (packetHeader & 0x80) // run-length packet
				{
					switch (targa_header.pixel_size)
					{
					case 24:
						blue = Buf_GetC (buf);
						green = Buf_GetC (buf);
						red = Buf_GetC (buf);
						alphabyte = 255;
						break;
					case 32:
						blue = Buf_GetC (buf);
						green = Buf_GetC (buf);
						red = Buf_GetC (buf);
						alphabyte = Buf_GetC (buf);
						break;
					default: /* avoid compiler warnings */
						blue = red = green = alphabyte = 0;
					}

					for (j = 0; j < packetSize; j++)
					{
						*pixbuf++ = red;
						*pixbuf++ = green;
						*pixbuf++ = blue;
						*pixbuf++ = alphabyte;
						column++;
						if (column == columns) // run spans across rows
						{
							column = 0;
							if (row > 0)
								row--;
							else
								goto breakOut;
							// johnfitz -- fix for upside-down targas
							realrow = upside_down ? row : rows - 1 - row;
							pixbuf = targa_rgba + realrow * columns * 4;
							// johnfitz
						}
					}
				}
				else // non run-length packet
				{
					for (j = 0; j < packetSize; j++)
					{
						switch (targa_header.pixel_size)
						{
						case 24:
							blue = Buf_GetC (buf);
							green = Buf_GetC (buf);
							red = Buf_GetC (buf);
							*pixbuf++ = red;
							*pixbuf++ = green;
							*pixbuf++ = blue;
							*pixbuf++ = 255;
							break;
						case 32:
							blue = Buf_GetC (buf);
							green = Buf_GetC (buf);
							red = Buf_GetC (buf);
							alphabyte = Buf_GetC (buf);
							*pixbuf++ = red;
							*pixbuf++ = green;
							*pixbuf++ = blue;
							*pixbuf++ = alphabyte;
							break;
						default: /* avoid compiler warnings */
							blue = red = green = alphabyte = 0;
						}
						column++;
						if (column == columns) // pixel packet run spans across rows
						{
							column = 0;
							if (row > 0)
								row--;
							else
								goto breakOut;
							// johnfitz -- fix for upside-down targas
							realrow = upside_down ? row : rows - 1 - row;
							pixbuf = targa_rgba + realrow * columns * 4;
							// johnfitz
						}
					}
				}
			}
		breakOut:;
		}
	}

	Buf_Free (buf);
	fclose (fin);

	*width = (int)(targa_header.width);
	*height = (int)(targa_header.height);
	return targa_rgba;
}

//==============================================================================
//
//  PCX
//
//==============================================================================

typedef struct
{
	char           signature;
	char           version;
	char           encoding;
	char           bits_per_pixel;
	unsigned short xmin, ymin, xmax, ymax;
	unsigned short hdpi, vdpi;
	byte           colortable[48];
	char           reserved;
	char           color_planes;
	unsigned short bytes_per_line;
	unsigned short palette_type;
	char           filler[58];
} pcxheader_t;

static int Image_PCXGetByte (stdio_buffer_t *buf, FILE *f, byte *data)
{
	int c = Buf_GetC (buf);
	if (c == EOF)
	{
		Buf_Free (buf);
		fclose (f);
		Mem_Free (data);
		Sys_Error ("'%s' is a truncated PCX file", loadfilename);
		return 0;
	}
	return c & 0xFF;
}

/*
============
Image_LoadPCX
============
*/
byte *Image_LoadPCX (FILE *f, int *width, int *height)
{
	pcxheader_t     pcx;
	int             x, y, w, h, readbyte, runlength, start;
	byte           *p, *data, *data_end;
	byte            palette[768];
	stdio_buffer_t *buf;

	start = ftell (f); // save start of file (since we might be inside a pak file, SEEK_SET might not be the start of the pcx)

	if (fread (&pcx, sizeof (pcx), 1, f) != 1)
		Sys_Error ("'%s' is not a valid PCX file", loadfilename);

	pcx.xmin = (unsigned short)LittleShort (pcx.xmin);
	pcx.ymin = (unsigned short)LittleShort (pcx.ymin);
	pcx.xmax = (unsigned short)LittleShort (pcx.xmax);
	pcx.ymax = (unsigned short)LittleShort (pcx.ymax);
	pcx.bytes_per_line = (unsigned short)LittleShort (pcx.bytes_per_line);

	if (pcx.signature != 0x0A)
		Sys_Error ("'%s' is not a valid PCX file", loadfilename);

	if (pcx.version != 5)
		Sys_Error ("'%s' is version %i, should be 5", loadfilename, pcx.version);

	if (pcx.encoding != 1 || pcx.bits_per_pixel != 8 || pcx.color_planes != 1)
		Sys_Error ("'%s' has wrong encoding or bit depth", loadfilename);

	w = pcx.xmax - pcx.xmin + 1;
	h = pcx.ymax - pcx.ymin + 1;

	data = (byte *)Mem_Alloc ((w * h + 1) * 4); //+1 to allow reading padding byte on last line
	data_end = data + ((size_t)w * (size_t)h + 1u) * 4u;

	// load palette
	fseek (f, start + com_filesize - 768, SEEK_SET);
	if (fread (palette, 1, 768, f) != 768)
	{
		fclose (f);
		Mem_Free (data);
		Sys_Error ("'%s' is not a valid PCX file", loadfilename);
	}

	// back to start of image data
	fseek (f, start + sizeof (pcx), SEEK_SET);

	buf = Buf_Alloc (f);

	for (y = 0; y < h; y++)
	{
		p = data + y * w * 4;

		for (x = 0; x < (pcx.bytes_per_line);) // read the extra padding byte if necessary
		{
			readbyte = Image_PCXGetByte (buf, f, data);

			if (readbyte >= 0xC0)
			{
				runlength = readbyte & 0x3F;
				readbyte = Image_PCXGetByte (buf, f, data);
			}
			else
				runlength = 1;

			while (runlength--)
			{
				if (p + 4 <= data_end)
				{
					p[0] = palette[readbyte * 3];
					p[1] = palette[readbyte * 3 + 1];
					p[2] = palette[readbyte * 3 + 2];
					p[3] = 255;
				}
				p += 4;
				x++;
			}
		}
	}

	Buf_Free (buf);
	fclose (f);

	*width = w;
	*height = h;
	return data;
}

//==============================================================================
//
//  STB_IMAGE_WRITE
//
//==============================================================================

static byte *CopyFlipped (const byte *data, int width, int height, int bpp)
{
	int   y, rowsize;
	byte *flipped;

	rowsize = width * (bpp / 8);
	flipped = (byte *)Mem_Alloc (height * rowsize);
	if (!flipped)
		return NULL;

	for (y = 0; y < height; y++)
	{
		memcpy (&flipped[y * rowsize], &data[(height - 1 - y) * rowsize], rowsize);
	}
	return flipped;
}

static void Image_PngStoreU32BE (byte out[4], uint32_t v)
{
	out[0] = (byte)((v >> 24) & 0xFFu);
	out[1] = (byte)((v >> 16) & 0xFFu);
	out[2] = (byte)((v >> 8) & 0xFFu);
	out[3] = (byte)(v & 0xFFu);
}

static uint32_t Image_PngCrcUpdate (uint32_t crc, const byte *data, size_t size)
{
	for (size_t i = 0; i < size; i++)
	{
		crc ^= (uint32_t)data[i];
		for (int bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return crc;
}

static uint32_t Image_PngAdler32 (const byte *data, size_t size)
{
	uint32_t a = 1u;
	uint32_t b = 0u;
	for (size_t i = 0; i < size; i++)
	{
		a = (a + (uint32_t)data[i]) % 65521u;
		b = (b + a) % 65521u;
	}
	return (b << 16) | a;
}

static void Image_CopyBytes (byte *dst, const byte *src, size_t len)
{
	for (size_t i = 0; i < len; i++)
		dst[i] = src[i];
}

static qboolean Image_PngWriteChunk (FILE *f, const char type[4], const byte *data, uint32_t len)
{
	byte header[8];
	byte crc_bytes[4];
	const byte type_bytes[4] = {(byte)type[0], (byte)type[1], (byte)type[2], (byte)type[3]};

	Image_PngStoreU32BE (header, len);
	Image_CopyBytes (header + 4, type_bytes, 4);

	uint32_t crc = Image_PngCrcUpdate (0xFFFFFFFFu, type_bytes, 4);
	if (len > 0 && data != NULL)
		crc = Image_PngCrcUpdate (crc, data, (size_t)len);
	Image_PngStoreU32BE (crc_bytes, crc ^ 0xFFFFFFFFu);

	if (fwrite (header, 1, sizeof (header), f) != sizeof (header))
		return false;
	if (len > 0 && data != NULL && fwrite (data, 1, (size_t)len, f) != (size_t)len)
		return false;
	return fwrite (crc_bytes, 1, sizeof (crc_bytes), f) == sizeof (crc_bytes);
}

static byte *Image_PngBuildIdat (const byte *pixels, int width, int height, int bpp_bytes, size_t *out_size)
{
	const size_t row_bytes = (size_t)width * (size_t)bpp_bytes;
	const size_t raw_size = (size_t)height * (1u + row_bytes);
	byte        *raw = (byte *)Mem_Alloc (raw_size);
	if (!raw)
		return NULL;

	for (int y = 0; y < height; y++)
	{
		byte *dst = raw + (size_t)y * (1u + row_bytes);
		dst[0] = 0u;
		Image_CopyBytes (dst + 1, pixels + (size_t)y * row_bytes, row_bytes);
	}

	const size_t block_count = (raw_size + 65534u) / 65535u;
	const size_t idat_cap = 2u + block_count * 5u + raw_size + 4u;
	if (idat_cap > 0xFFFFFFFFu)
	{
		Mem_Free (raw);
		return NULL;
	}

	byte *idat = (byte *)Mem_Alloc (idat_cap);
	if (!idat)
	{
		Mem_Free (raw);
		return NULL;
	}

	size_t pos = 0;
	idat[pos++] = 0x78u;
	idat[pos++] = 0x01u;

	size_t offset = 0;
	while (offset < raw_size)
	{
		const size_t   rem = raw_size - offset;
		const size_t   block = rem > 65535u ? 65535u : rem;
		const uint16_t length = (uint16_t)block;
		const uint16_t inverse = (uint16_t)(~length);

		idat[pos++] = (offset + block >= raw_size) ? 1u : 0u;
		idat[pos++] = (byte)(length & 0xFFu);
		idat[pos++] = (byte)((length >> 8) & 0xFFu);
		idat[pos++] = (byte)(inverse & 0xFFu);
		idat[pos++] = (byte)((inverse >> 8) & 0xFFu);
		Image_CopyBytes (idat + pos, raw + offset, block);
		pos += block;
		offset += block;
	}

	Image_PngStoreU32BE (idat + pos, Image_PngAdler32 (raw, raw_size));
	pos += 4u;
	Mem_Free (raw);
	*out_size = pos;
	return idat;
}

static qboolean Image_PngWriteFile (const char *pathname, int width, int height, int bpp,
                                    const byte *idat, uint32_t idat_size)
{
	FILE *f = fopen (pathname, "wb");
	if (!f)
		return false;

	const byte signature[8] = {137u, 80u, 78u, 71u, 13u, 10u, 26u, 10u};
	byte       ihdr[13] = {0};
	Image_PngStoreU32BE (ihdr, (uint32_t)width);
	Image_PngStoreU32BE (ihdr + 4, (uint32_t)height);
	ihdr[8] = 8u;
	ihdr[9] = (byte)(bpp == 32 ? 6u : 2u);

	qboolean ok = fwrite (signature, 1, sizeof (signature), f) == sizeof (signature);
	ok = ok && Image_PngWriteChunk (f, "IHDR", ihdr, (uint32_t)sizeof (ihdr));
	ok = ok && Image_PngWriteChunk (f, "IDAT", idat, idat_size);
	ok = ok && Image_PngWriteChunk (f, "IEND", NULL, 0u);
	if (fflush (f) != 0)
		ok = false;
	if (fclose (f) != 0)
		ok = false;
	return ok;
}

/*
============
Image_WritePNG -- writes an uncompressed PNG (stored DEFLATE) to com_gamedir

returns true if successful
============
*/
qboolean Image_WritePNG (const char *name, byte *data, int width, int height, int bpp, qboolean upsidedown)
{
	char   pathname[MAX_OSPATH];
	byte  *flipped;
	byte  *idat;
	size_t idat_size = 0;

	if (!(bpp == 32 || bpp == 24) || width <= 0 || height <= 0 || !data)
		return false;

	Sys_mkdir (com_gamedir); // if we've switched to a nonexistant gamedir, create it now so we don't crash
	q_snprintf (pathname, sizeof (pathname), "%s/%s", com_gamedir, name);

	flipped = upsidedown ? data : CopyFlipped (data, width, height, bpp);
	if (!flipped)
		return false;

	idat = Image_PngBuildIdat (flipped, width, height, bpp / 8, &idat_size);
	if (!upsidedown)
		Mem_Free (flipped);
	if (!idat)
		return false;

	const qboolean ok = Image_PngWriteFile (pathname, width, height, bpp, idat, (uint32_t)idat_size);
	Mem_Free (idat);
	return ok;
}

/*
============
Image_WriteJPG -- writes using stb_image_write

returns true if successful
============
*/
qboolean Image_WriteJPG (const char *name, byte *data, int width, int height, int bpp, int quality, qboolean upsidedown)
{
	unsigned error;
	char     pathname[MAX_OSPATH];
	byte    *flipped;
	int      bytes_per_pixel;

	if (!(bpp == 32 || bpp == 24))
		Sys_Error ("bpp not 24 or 32");

	bytes_per_pixel = bpp / 8;

	Sys_mkdir (com_gamedir); // if we've switched to a nonexistant gamedir, create it now so we don't crash
	q_snprintf (pathname, sizeof (pathname), "%s/%s", com_gamedir, name);

	if (!upsidedown)
	{
		flipped = CopyFlipped (data, width, height, bpp);
		if (!flipped)
			return false;
	}
	else
		flipped = data;

	error = stbi_write_jpg (pathname, width, height, bytes_per_pixel, flipped, quality);
	if (!upsidedown)
		Mem_Free (flipped);

	return (error != 0);
}
