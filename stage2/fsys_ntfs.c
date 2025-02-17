/*
 *  NTFS file system driver for GRUB
 *
 *  Copyright (C) 2007 Bean (bean123@126.com)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 * Limitations:
 *  1. Don't support >1K MFT record size, >4K INDEX record size
 *  2. Don't support encrypted file
 *  3. Don't support >4K non-resident attribute list and $BITMAP
 *	2014.06.01 Support <=8K non-resident attribute list
 *	2015.04.27 Support to write resident attribute data(<900 byte files)
 *	2015.05.13 Support arbitrary length non-resident attribute list
 */

#ifdef FSYS_NTFS

#include "shared.h"
#include "filesys.h"
#include "iamath.h"
#include "term.h"

//#define NTFS_DEBUG	1

#ifdef FS_UTIL
#include "fsutil.h"
#endif

#define FILE_MFT      0
#define FILE_MFTMIRR  1
#define FILE_LOGFILE  2
#define FILE_VOLUME   3
#define FILE_ATTRDEF  4
#define FILE_ROOT     5
#define FILE_BITMAP   6
#define FILE_BOOT     7
#define FILE_BADCLUS  8
#define FILE_QUOTA    9
#define FILE_UPCASE  10

#define AT_STANDARD_INFORMATION	0x10
#define AT_ATTRIBUTE_LIST	0x20
#define AT_FILENAME		0x30
#define AT_OBJECT_ID		0x40
#define AT_SECURITY_DESCRIPTOR	0x50
#define AT_VOLUME_NAME		0x60
#define AT_VOLUME_INFORMATION	0x70
#define AT_DATA			0x80
#define AT_INDEX_ROOT		0x90
#define AT_INDEX_ALLOCATION	0xA0
#define AT_BITMAP		0xB0
#define AT_SYMLINK		0xC0
#define AT_EA_INFORMATION	0xD0
#define AT_EA			0xE0

#define ATTR_READ_ONLY		0x1
#define ATTR_HIDDEN		0x2
#define ATTR_SYSTEM		0x4
#define ATTR_ARCHIVE		0x20
#define ATTR_DEVICE		0x40
#define ATTR_NORMAL		0x80
#define ATTR_TEMPORARY		0x100
#define ATTR_SPARSE		0x200
#define ATTR_REPARSE		0x400
#define ATTR_COMPRESSED		0x800
#define ATTR_OFFLINE		0x1000
#define ATTR_NOT_INDEXED	0x2000
#define ATTR_ENCRYPTED		0x4000
#define ATTR_DIRECTORY		0x10000000
#define ATTR_INDEX_VIEW		0x20000000

#define FLAG_COMPRESSED		1
#define FLAG_ENCRYPTED		0x4000
#define FLAG_SPARSE		0x8000

#define BLK_SHR		9

#define MAX_MFT		(1024 >> BLK_SHR)
#define MAX_IDX		(16384 >> BLK_SHR)

#define valueat(buf,ofs,type)	*((type*)(((char*)buf)+ofs))

#define AF_ALST		1
#define AF_GPOS		2

#define RF_COMP		1
#define RF_CBLK		2
#define RF_BLNK		4

#define set_aflag(a,b)	if (b) attr_flg|=(a); else attr_flg&=~(a);
#define get_aflag(a)	(attr_flg & (a))

#define set_rflag(a,b)	if (b) ctx->flags|=(a); else ctx->flags&=~(a);
#define get_rflag(a)	(ctx->flags & (a))

static unsigned long mft_size,idx_size,spc,blocksize,mft_start;
static unsigned char log2_bps, log2_bpc, log2_spc, file_backup[48];

typedef struct {
  int flags;
  unsigned long target_vcn,curr_vcn,next_vcn,curr_lcn;
  unsigned long vcn_offset;
  char *mft,*cur_run;
} read_ctx;

/* sbuf must be at 4K boundary!! read_block() require this.*/
#define NAME_BUF	((char *)(FSYS_BUF))	/* 4096 bytes */
#define TEMP_BUF	NAME_BUF		/* 4096 bytes */
#define mmft		((char *)((FSYS_BUF)+4096))
#define cmft		(mmft+1024+1024+4096)
#define sbuf		(cmft+1024+1024+4096)	/* 4096 bytes */
#define cbuf		(sbuf+4096)		/* 4096 bytes */

#define attr_flg	valueat(cur_mft,0,unsigned short)

#define attr_cur	valueat(cur_mft,2,unsigned short)
#define attr_nxt	valueat(cur_mft,4,unsigned short)
#define attr_end	valueat(cur_mft,6,unsigned short)

#define save_pos	valueat(cur_mft,8,unsigned long long)
#define list_len	valueat(cur_mft,16,unsigned short)
#define list_ofs	valueat(cur_mft,18,unsigned short)

#define emft_buf	(cur_mft+1024)
#define edat_buf	(cur_mft+2048)

#define ofs2ptr(a)	(cur_mft+(a))
#define ptr2ofs(a)	((unsigned short)((a)-cur_mft))

//#ifdef NTFS_DEBUG
//#define dbg_printf	printf
//#else
//#define dbg_printf	if (0) printf
//#endif
#define dbg_printf	if (((unsigned long)debug) >= 0x7FFFFFFF) printf

static int fixup(char* buf,int len,char* magic,int tag)
{
  int ss;
  char *pu, *qu;
  unsigned us;

	if (tag)
	{
		grub_memmove64 ((unsigned long long)(unsigned int)buf,(unsigned long long)(unsigned int)file_backup,20);
	}
	else
	{
		grub_memmove64 ((unsigned long long)(unsigned int)file_backup,(unsigned long long)(unsigned int)buf,48);
	}	
	
  if (valueat(buf,0,unsigned long)!=valueat(magic,0,unsigned long))
    {
      dbg_printf("%s label not found\n",magic);
      return 0;
    }

  ss=valueat(buf,6,unsigned short)-1;
  if (ss*blocksize!=len*512)
    {
      dbg_printf("Size not match %d!=%d\n",(ss*blocksize),(len*512));
      return 0;
    }
  qu=pu=buf+valueat(buf,4,unsigned short);
  us=valueat(pu,0,unsigned short);
  buf-=2;
  while (ss>0)
    {
      buf+=blocksize;
      pu+=2;
			if (tag)
			{
				valueat(pu,0,unsigned short)=valueat(buf,0,unsigned short);
				valueat(buf,0,unsigned short)=valueat(qu,0,unsigned short);
			}
			else
			{
				if (valueat(buf,0,unsigned short)!=us)
        {
          dbg_printf("Fixup signature not match\n");
          return 0;
        }
      valueat(buf,0,unsigned short)=valueat(pu,0,unsigned short);
			}
      ss--;
    }
  return 1;
}

/*static*/ int read_mft(char* buf,unsigned long mftno);
static int read_attr(char* cur_mft,unsigned long long dest,unsigned long long ofs,unsigned long long len,int cached,unsigned long write);
static int read_data(char* cur_mft,char* pa,unsigned long long dest,unsigned long long ofs,unsigned long long len,int cached,unsigned long write);
static int read_list(char* cur_mft,char* pa,int cached,unsigned long write);

static void init_attr(char* cur_mft)
{
  attr_flg=0;
  attr_nxt=ptr2ofs(cur_mft+valueat(cur_mft,0x14,unsigned short));
  attr_end=0;
	list_len=0;
	list_ofs=0;
}

static char* find_attr(char* cur_mft,unsigned char attr)
{
  char* pa;

  if (get_aflag(AF_ALST))
    {
back:
      while (attr_nxt<attr_end)
        {
          attr_cur=attr_nxt;
          pa=ofs2ptr(attr_cur);
          attr_nxt+=valueat(pa,4,unsigned short);
          if (((unsigned char)*pa==attr) || (attr==0))
            {
              char *new_pos;

              if (cur_mft==mmft)
                {
                  if ((! devread(valueat(pa,0x10,unsigned long),0,512,(unsigned long long)(unsigned int)emft_buf, 0xedde0d90)) ||
                      (! devread(valueat(pa,0x14,unsigned long),0,512,(unsigned long long)(unsigned int)(emft_buf+512), 0xedde0d90)))
                    {
                      dbg_printf("Read Error\n");
                      return NULL;
                    }

                  if (! fixup(emft_buf,mft_size,"FILE",0))
                    {
                      dbg_printf("Invalid MFT at 0x%X\n",(valueat(pa,0x10,unsigned long)));
                      return NULL;
                    }
                }
              else
                {
                  if (! read_mft(emft_buf,valueat(pa,0x10,unsigned long)))
                    return NULL;
                }

              new_pos=&emft_buf[valueat(emft_buf,0x14,unsigned short)];
              while ((unsigned char)*new_pos!=0xFF)
                {
                  if (((unsigned char)*new_pos==(unsigned char)*pa) &&
                      (valueat(new_pos,0xE,unsigned short)==valueat(pa,0x18,unsigned short)))
                    {
                      return new_pos;
                    }
                  new_pos+=valueat(new_pos,4,unsigned long);
                }
              dbg_printf("Can\'t find 0x%X in attribute list\n",(unsigned long)(unsigned char)*pa);
              return NULL;
            }
        }
			if (list_len)
			{
				list_ofs += 4096;
				attr_nxt=ptr2ofs(cur_mft+valueat(cur_mft,0x14,unsigned short));
				pa=ofs2ptr(attr_nxt);
				while ((unsigned char)*pa!=0xFF)
				{
					attr_cur=attr_nxt;
					attr_nxt+=valueat(pa,4,unsigned long);
					pa=ofs2ptr(attr_nxt);
					if ((unsigned char)*pa==0x20)
						break;
				}
				if (! read_list(cur_mft,pa,0,0xedde0d90))
					return NULL;
				goto back;
			}
      return NULL;
    }
  pa=ofs2ptr(attr_nxt);
  while ((unsigned char)*pa!=0xFF)
    {
      attr_cur=attr_nxt;
      attr_nxt+=valueat(pa,4,unsigned long);
      if ((unsigned char)*pa==AT_ATTRIBUTE_LIST)
        attr_end=attr_cur;
      if (((unsigned char)*pa==attr) || (attr==0))
        return pa;
      pa=ofs2ptr(attr_nxt);
    }
  if (attr_end)
    {
      pa=ofs2ptr(attr_end);
      if (pa[8])
        {
          attr_cur=attr_end;
				list_len = valueat(pa,0x30,unsigned long);
				if (! read_list(cur_mft,pa,0,0xedde0d90))
					return NULL;
        }
      else
        {
          attr_nxt=attr_end+valueat(pa,0x14,unsigned short);
          attr_end=attr_end+valueat(pa,4,unsigned long);
        }
      set_aflag(AF_ALST,1);
      while (attr_nxt<attr_end)
        {
          pa=ofs2ptr(attr_nxt);
          if (((unsigned char)*pa==attr) || (attr==0))
            break;
          attr_nxt+=valueat(pa,4,unsigned long);
        }
      if (attr_nxt>=attr_end)
        return NULL;

      if ((cur_mft==mmft) && (attr==AT_DATA))
        {
          unsigned short new_pos;

          set_aflag(AF_GPOS,1);
          attr_cur=attr_nxt;
          pa=ofs2ptr(attr_cur);
          valueat(pa,0x10,unsigned long)=mft_start;
          valueat(pa,0x14,unsigned long)=mft_start+1;
          new_pos=attr_nxt+valueat(pa,4,unsigned short);
          while (new_pos<attr_end)
            {
              pa=ofs2ptr(new_pos);
              if ((unsigned char)*pa!=attr)
                break;
              if (! read_attr(cur_mft,(unsigned long long)(unsigned int)(pa+0x10),valueat(pa,0x10,unsigned long)*(mft_size << BLK_SHR),((unsigned long long)(mft_size)) << BLK_SHR,0, 0xedde0d90))
                return NULL;
              new_pos+=valueat(pa,4,unsigned short);
            }
          attr_nxt=attr_cur;
          set_aflag(AF_GPOS,0);
        }
      goto back;
    }
  return NULL;
}

static int read_list(char* cur_mft,char* pa,int cached,unsigned long write)
{
	unsigned long long n;
	if (list_len > 4096)
	{
		attr_end=ptr2ofs(edat_buf+4096);
		n = 4096;
		list_len -= 4096;
	}
	else
	{
		attr_end=ptr2ofs(edat_buf+list_len);
		n = (list_len + blocksize -1) & (~(blocksize -1));
		list_len = 0;
	}
	if (! read_data(cur_mft,pa,(unsigned long long)(unsigned int)edat_buf,(unsigned long long)(unsigned int)list_ofs,n,cached, write))
	{
		dbg_printf("Fail to read non-resident attribute list\n");
		return 0;
	}
	attr_nxt=ptr2ofs(edat_buf);
	return 1;
}

static char* locate_attr(char* cur_mft,unsigned char attr)
{
  char* pa;

  init_attr(cur_mft);
  if ((pa=find_attr(cur_mft,attr))==NULL)
    return NULL;
  if (! get_aflag(AF_ALST))
    {
      while (1)
        {
          if ((pa=find_attr(cur_mft,attr))==NULL)
            break;
          if (get_aflag(AF_ALST))
            return pa;
        }
      init_attr(cur_mft);
      pa=find_attr(cur_mft,attr);
    }
  return pa;
}

static char* read_run_data(char* run,int nn,unsigned long* val,int sig)
{
  unsigned long r, v;

  r = 0;
  v = 1;

  while (nn--)
    {
      r += v * (*(unsigned char *)(run++));
      v <<= 8;
    }

  if ((sig) && (r & (v>>1)))
    r -=v;

  *val=r;
  return run;
}

static char* read_run_list(read_ctx* ctx,char* run)
{
  int c1,c2;
  unsigned long val;

back:
  c1=((unsigned char)(*run) & 0xF);
  c2=((unsigned char)(*run) >> 4);
  if (! c1)
    {
      char *cur_mft;

      cur_mft=ctx->mft;
      if ((cur_mft) && (get_aflag(AF_ALST)))
        {
          void (*save_hook)(unsigned long long, unsigned long, unsigned long long);

          save_hook=disk_read_func;
          disk_read_func=NULL;
          run=find_attr(cur_mft,(unsigned char)*ofs2ptr(attr_cur));
          disk_read_func=save_hook;
          if (run)
            {
              if (run[8]==0)
                {
                  dbg_printf("$DATA should be non-resident\n");
                  return NULL;
                }
              run+=valueat(run,0x20,unsigned short);
              ctx->curr_lcn=0;
              goto back;
            }
        }
      dbg_printf("Run list overflow\n");
      return NULL;
    }
  run=read_run_data(run+1,c1,&val,0);	// length of current VCN
  ctx->curr_vcn=ctx->next_vcn;
  ctx->next_vcn+=val;
  run=read_run_data(run,c2,&val,1);	// offset to previous LCN
  ctx->curr_lcn+=val;
  set_rflag(RF_BLNK,(val==0));
  return run;
}

static unsigned long comp_table[16][2];
static int comp_head,comp_tail,cbuf_ofs,cbuf_vcn;

static int decomp_nextvcn(void)
{
  if (comp_head>=comp_tail)
    {
      dbg_printf("C1\n");
      return 0;
    }
  if (! devread((comp_table[comp_head][1]-(comp_table[comp_head][0]-cbuf_vcn))*spc,0,spc << BLK_SHR,(unsigned long long)(unsigned int)cbuf, 0xedde0d90))
    {
      dbg_printf("Read Error\n");
      return 0;
    }
  cbuf_vcn++;
  if ((cbuf_vcn>=comp_table[comp_head][0]))
    comp_head++;
  cbuf_ofs=0;
  return 1;
}

static int decomp_getch(void)
{
  if (cbuf_ofs>=(spc << BLK_SHR))
    {
      if (! decomp_nextvcn())
        return 0;
    }
  return (unsigned char)cbuf[cbuf_ofs++];
}

// Decompress a block (4096 bytes)
static int decomp_block(char* dest)
{
  unsigned short flg,cnt;

  flg=decomp_getch();
  flg+=decomp_getch()*256;
  cnt=(flg & 0xFFF)+1;

  if (dest)
    {
      if (flg & 0x8000)
        {
          unsigned long bits,copied,tag;

          bits=copied=tag=0;
          while (cnt > 0)
            {
              if (! bits)
                {
                  tag = decomp_getch();
                  bits = 8;
                  cnt--;
                  if (cnt<=0)
                    break;
                }
              if (tag & 1)
                {
                  unsigned long i, len, delta, code, lmask, dshift;

                  code=decomp_getch();
                  code+=decomp_getch()*256;
                  cnt-=2;

                  if (! copied)
                    {
                      dbg_printf("B2\n");
                      return 0;
                    }

                  for (i = copied - 1, lmask = 0xFFF, dshift = 12; i >= 0x10; i >>= 1)
                    {
                      lmask >>= 1;
                      dshift--;
                    }

                  delta = code >> dshift;
                  len = (code & lmask) + 3;

		  if (copied + len > 4096)
		    {
		      dbg_printf("B3\n");
		      return 0;
		    }

                  for (i = 0; i < len; i++)
                    {
                      dest[copied] = dest[copied - delta - 1];
                      copied++;
                    }
                } else
                {
		  if (copied > 4096 - 1)
		    {
		      dbg_printf("B4\n");
		      return 0;
		    }

                  dest[copied++] = decomp_getch();
                  cnt--;
                }
              tag >>= 1;
              bits--;
            }
          return 1;
        }
      else
        {
          if (cnt!=4096)
            {
              dbg_printf("B3\n");
              return 0;
            }
        }
    }

  while (cnt>0)
    {
      int n;

      n=(spc << BLK_SHR) - cbuf_ofs;
      if (n>cnt)
        n=cnt;
      if ((dest) && (n))
        {
          memcpy(dest,&cbuf[cbuf_ofs],n);
          dest+=n;
        }
      cnt-=n;
      cbuf_ofs+=n;
      if ((cnt) && (! decomp_nextvcn()))
        return 0;
    }
  return 1;
}

static int read_block(read_ctx* ctx, unsigned long long buf, unsigned long num, unsigned long long len, unsigned long write)
{
  if (get_rflag(RF_COMP))
  {
      unsigned long cpb=(8/spc);

      if (write == 0x900ddeed)
	{
		grub_printf ("Fatal: Cannot write compressed file.\n");
		return 0;
	}

      while (num)
        {
          unsigned long nn;

          if ((ctx->target_vcn & 0xF)==0)
            {
              if (comp_head!=comp_tail)
                {
                  dbg_printf("A1\n");
                  return 0;
                }
              comp_head=comp_tail=0;
              cbuf_vcn=ctx->target_vcn;
              cbuf_ofs=(spc<<BLK_SHR);
              if (ctx->target_vcn>=ctx->next_vcn)
                {
                  ctx->cur_run=read_run_list(ctx,ctx->cur_run);
                  if (ctx->cur_run==NULL)
                    return 0;
                }
              while (ctx->target_vcn+16>ctx->next_vcn)
                {
                  if (get_rflag(RF_BLNK))
                    break;
                  comp_table[comp_tail][0]=ctx->next_vcn;
                  comp_table[comp_tail][1]=ctx->curr_lcn + ctx->next_vcn - ctx->curr_vcn;
                  comp_tail++;
                  ctx->cur_run=read_run_list(ctx,ctx->cur_run);
                  if (ctx->cur_run==NULL)
                    return 0;
                }
              //if (ctx->target_vcn+16<ctx->next_vcn)
              //  {
              //    dbg_printf("A2\n");
              //    return 0;
              //  }
            }

          nn=(16 - (ctx->target_vcn & 0xF)) / cpb;
          if (nn>num)
            nn=num;
          num-=nn;

          if (get_rflag(RF_BLNK))
            {
              ctx->target_vcn+=nn * cpb;
              if (comp_tail==0)
                {
                  if (buf)
                    {
                      grub_memset64 (buf,0,nn*4096);
                      buf+=nn*4096;
                    }
                }
              else
                {
                  while (nn)
                    {
		      char *dest = TEMP_BUF;

                      if (! decomp_block(dest))
                        return 0;
                      if (buf)
		      {
			grub_memmove64 (buf, (unsigned long long)(unsigned int)dest, 4096);
                        buf+=4096;
		      }
                      nn--;
                    }
                }
            }
          else
            {
              nn*=cpb;
              while ((comp_head<comp_tail) && (nn))
                {
                  int tt;

                  tt=comp_table[comp_head][0] - ctx->target_vcn;
                  if (tt>nn)
                    tt=nn;
                  ctx->target_vcn+=tt;
                  if (buf)
                    {
                      if (! devread((comp_table[comp_head][1]-(comp_table[comp_head][0] - ctx->target_vcn))*spc,0,(unsigned long long)tt*(spc << BLK_SHR),buf, 0xedde0d90))
                        {
                          dbg_printf("Read Error\n");
                          return 0;
                        }
                      buf+=(unsigned long long)tt*(spc << BLK_SHR);
                    }
                  nn-=tt;
                  if (ctx->target_vcn>=comp_table[comp_head][0])
                    comp_head++;
                }
              if (nn)
                {
                  if (buf)
                    {
                      if (! devread((ctx->target_vcn - ctx->curr_vcn + ctx->curr_lcn)*spc,0,(unsigned long long)nn*(spc << BLK_SHR),buf, 0xedde0d90))
                        {
                          dbg_printf("Read Error\n");
                          return 0;
                        }
                      buf+=(unsigned long long)nn*(spc << BLK_SHR);
                    }
                  ctx->target_vcn+=nn;
                }
            }
        }
  }
  else
  {
      while (num)
      {
	  unsigned long nn;
	  unsigned long long ss;

	  nn = (ctx->next_vcn - ctx->target_vcn) * spc - ctx->vcn_offset;

	  if (nn > num)
	      nn = num;

	  if (len && nn)
	  {
		if (get_rflag (RF_BLNK))
		{
			if (write == 0x900ddeed)
			{
				grub_printf ("Fatal: Cannot write NULL blocks.\n");
				return 0;
			}
			if (buf)
				grub_memset64 (buf, 0, (unsigned long long)nn << BLK_SHR);
		}
		else
		{
			unsigned long s = (ctx->target_vcn - ctx->curr_vcn + ctx->curr_lcn) * spc + ctx->vcn_offset;
			unsigned long o = 0;

			if (write != 0x900ddeed)	/* read */
				ss = ((unsigned long long)nn << BLK_SHR);
 			else if (len == -1ULL)	/* long long of -1 for normal whole-block writing */
				ss = ((unsigned long long)nn << BLK_SHR);
			else if ((long long)len < 0)	/* long long of -2, -3, ... for writing a piece of block */
			{
				ss = -len;
				ss--;
				if (ss >= 512)
				{
					grub_printf ("Fatal! ss(=%ld) should not be >= 512.\n", ss);
					return 0;
				}
				//o = 512 - (unsigned long)len;

				/* sbuf must be 4K align!! buf is now offset to sbuf. */
				//o = ((unsigned long)buf) % 4096;  //sbuf必须是4K对齐
				o = (unsigned int)filepos % (1UL << log2_bps); //2022-07-15  解决写偏移的问题   sbuf无需4K对齐
			}
			else {
				ss = len;
			}
			if (! devread(s, o, ss, buf, write))
			{
				dbg_printf("Read/Write Error\n");
				return 0;
			}
		}
		if (buf)
			buf += ((unsigned long long)nn << BLK_SHR);
	  }
	  //ss = ctx->target_vcn * spc + ctx->vcn_offset + nn;
	  //ctx->target_vcn = ss / spc;
	  //ctx->vcn_offset = ss % spc;
	  ss = (ctx->target_vcn << log2_spc) + ctx->vcn_offset + nn;
	  ctx->target_vcn = ((unsigned long)ss) >> log2_spc;
	  ctx->vcn_offset = ((unsigned long)ss) & (spc-1);
	  num -= nn;
	  if (num == 0)
		break;

          if (ctx->target_vcn >= ctx->next_vcn)
	  {
		ctx->cur_run = read_run_list (ctx, ctx->cur_run);
		if (ctx->cur_run == NULL)
			return 0;
	  }
      }
  }
  return 1;
}

static int read_data(char* cur_mft,char* pa,unsigned long long dest,unsigned long long ofs,unsigned long long len,int cached,unsigned long write)
{
    unsigned long vcn, blk_size;
    unsigned char log2_blk_size;
    read_ctx cc={0}, *ctx;
    int ret=0;

    if (len == 0)
	return 1;

    ctx = &cc;

    if (pa[8] == 0)
    {
	if (write == 0x900ddeed)	/* write */
	{
//		grub_printf ("Fatal: Cannot write resident/small file! Enlarge it to 2KB and try again.\n");
//		return 0;
		if (valueat(file_backup,0x2c,unsigned long) != valueat(cur_mft,0x2c,unsigned long))
			goto fail;
		grub_memmove64 (((unsigned long long)(unsigned int)(pa + valueat(pa,0x14,unsigned long))+ofs),dest,len);
		fixup(cur_mft,mft_size,"FILE",1);
		if (! devread(mft_start + valueat(cur_mft,0x2c,unsigned long) * mft_size,0,mft_size << log2_bps,(unsigned long long)(unsigned int)cur_mft,0x900ddeed))
			goto fail;
		return 1;
	}
	if (ofs + len > valueat(pa,0x10,unsigned long))
	{
		dbg_printf("Read out of range\n");
		return 0;
	}
	if (dest)
		grub_memmove64 (dest, (unsigned long long)(unsigned int)(pa + valueat(pa,0x14,unsigned long) + ofs), len);
	
	disk_read_func = disk_read_hook;
	devread(mft_start + valueat(cur_mft,0x2c,unsigned long) * mft_size, pa - cur_mft + valueat(pa,0x14,unsigned long), len, 0, GRUB_LISTBLK);
		disk_read_func = NULL;
	return 1;
    }

    ctx->mft = cur_mft;
    set_rflag(RF_COMP, valueat(pa,0xC,unsigned short) & FLAG_COMPRESSED);
    ctx->cur_run = pa + valueat(pa,0x20,unsigned short);
    //blk_size = (get_rflag(RF_COMP)) ? 4096 : 512;
    log2_blk_size = (get_rflag(RF_COMP)) ? 12 : 9 ;
    blk_size = 1UL << log2_blk_size;

    if ((get_rflag(RF_COMP)) && (! cached))
    {
	dbg_printf("Attribute can\'t be compressed\n");
	return 0;
    }

    if (cached && write != 0x900ddeed)	/* read */
    {
	//if ((ofs & (~(blk_size - 1))) == save_pos)
	if ((ofs & (-(unsigned long long)blk_size)) == save_pos)
	{
		unsigned long n,bofs;

		//if (write == 0x900ddeed)	/* write */
		//{
		//	grub_printf ("Fatal: Cannot write file with save_pos!\n");
		//	return 0;
		//}
		bofs = (unsigned long)ofs - (unsigned long)save_pos;
		n = blk_size - bofs;
		if (n > len)
		    n = len;

		if (dest)
		{
//			if (write == 0x900ddeed)	/* write */
//			memcpy (sbuf + ofs - save_pos, dest, n);
//			else
			grub_memmove64 (dest, (unsigned long long)(unsigned long)(sbuf + bofs), n);
		}
		if (n == len)
			return 1;

		if (dest)
			dest += n;
		len -= n;
		ofs += n;
	}
    }

    if (get_rflag(RF_COMP))
    {
	vcn = ctx->target_vcn = (ofs & (-4096ULL)) >> log2_bpc;
	ctx->vcn_offset = 0;
	ctx->target_vcn &= ~0xF;
	comp_head = comp_tail = 0;
    }
    else
    {
	//vcn = ctx->target_vcn = (ofs >> BLK_SHR) / spc;
	//ctx->vcn_offset = (ofs >> BLK_SHR) % spc;
	vcn = ctx->target_vcn = ofs >> log2_bpc;
	ctx->vcn_offset = (ofs >> BLK_SHR) & (spc-1);
    }

    ctx->next_vcn = valueat(pa,0x10,unsigned long);
    ctx->curr_lcn = 0;
    while (ctx->next_vcn <= ctx->target_vcn)
    {
	ctx->cur_run = read_run_list (ctx, ctx->cur_run);
	if (ctx->cur_run == NULL)
		return 0;
    }

    if (get_aflag(AF_GPOS))
    {
	unsigned long tmp1, tmp2;

	tmp2 = tmp1 = (ctx->target_vcn - ctx->curr_vcn + ctx->curr_lcn) * spc + ctx->vcn_offset;
	tmp2++;
	if (dest)
	{
		valueat((char *)(unsigned int)dest,0,unsigned long) = tmp1;
		valueat((char *)(unsigned int)dest,4,unsigned long) = tmp2;
	}
	if (tmp2 == (ctx->next_vcn - ctx->curr_vcn + ctx->curr_lcn) * spc)
	{
		ctx->cur_run = read_run_list (ctx, ctx->cur_run);
		if (ctx->cur_run == NULL)
			return 0;
		if (dest)
			valueat((char *)(unsigned int)dest,4,unsigned long) = ctx->curr_lcn * spc;
	}
	return 1;
    }

    if ((vcn > ctx->target_vcn) &&
	//(! read_block (ctx, 0ULL, ((vcn - ctx->target_vcn) * spc) / 8, 0, 0xedde0d90)))
	(! read_block (ctx, 0ULL, ((vcn - ctx->target_vcn) << log2_spc) >> 3, 0, 0xedde0d90)))
	return 0;

 //   ret = 0;

    if ((cached) && (valueat(pa,0xC,unsigned short) & (FLAG_COMPRESSED + FLAG_SPARSE))==0)
	disk_read_func = disk_read_hook;
    else if (write == 0x900ddeed)	/* write */
    {
	grub_printf("Fatal: Cannot write compressed or sparse file!\n");
	goto fail;
    }

    /* read the beginning piece of data(if any) upto a block boundary. */
    //if (ofs % blk_size)
    if ((unsigned long)ofs & (blk_size-1))
    {
	unsigned long long t;
	unsigned long n, o;

	if (! cached)
	{
		dbg_printf("Invalid range\n");
		goto fail;
	}

	o = ofs & (blk_size-1);
	n = blk_size - o;
	if (n > len)
	    n = len;

	if (dest && write == 0x900ddeed)	/* write */
		grub_memmove64 ((unsigned long long)(unsigned int)&sbuf[o], dest, n);

	//t = ctx->target_vcn * (spc << BLK_SHR);
	t = (unsigned long long)(ctx->target_vcn) << log2_bpc;
	//if (! read_block (ctx, sbuf, 1, -1, 0xedde0d90))	/* read */
	/* (-n-1) is long long value ranging from -2, -3, ..., -blk_size */
	/* sbuf must be 4K align !! */
	if (! read_block (ctx, (unsigned long long)(unsigned int)(write == 0x900ddeed ? &sbuf[o] : sbuf), 1, ((-(long long)n)-1), write))	/* read/write */
		goto fail;

	if (write != 0x900ddeed)	/* read */
	{
		save_pos = t;
		if (dest)
		{
			//if (write == 0x900ddeed)	/* write */
			//{
			//    if (grub_memcmp (dest, &sbuf[o], n) == 0)
			//	goto next;
			//    memcpy (&sbuf[o], dest, n);
			//    if (! read_block (ctx, sbuf, 1, -1, write))	/* write */
			//	goto fail;
			//    goto next;
			//}
			grub_memmove64 (dest, (unsigned long long)(unsigned int)&sbuf[o], n);
		}
	}
//next:
	if (n == len)
		goto done;
	if (dest)
		dest += n;
	len -= n;
    }

    //if (! read_block (ctx, dest, ((unsigned int)len) / blk_size, -1ULL, write)) /* read/write */	/* XXX: 64-bit ? */
    if (! read_block (ctx, dest, len >> log2_blk_size, -1ULL, write)) /* read/write */
	goto fail;

    if (dest)
	//dest += (((unsigned int)len) / blk_size) * blk_size;	/* XXX: 64-bit ? */
	dest += len & (-(unsigned long long)blk_size);

    //len = ((unsigned int)len) % blk_size;	/* XXX: 64-bit ? */
    len = ((unsigned long)len) & (blk_size-1);

    if (len)
    {
	unsigned long long t;

	if (! cached)
	{
		dbg_printf("Invalid range\n");
		goto fail;
	}

	if (dest && write == 0x900ddeed)	/* write */
		grub_memmove64 ((unsigned long long)(unsigned int)sbuf, dest, len);

	//t = ctx->target_vcn * (spc << BLK_SHR);
	t = (unsigned long long)(ctx->target_vcn) << log2_bpc;
	if (! read_block (ctx, (unsigned long long)(unsigned int)sbuf, 1, len, write))	/* read/write */
		goto fail;

	if (write != 0x900ddeed)	/* read */
	{
		save_pos = t;
		if (dest)
		{
			//if (write == 0x900ddeed)	/* write */
			//{
			//	if (grub_memcmp (dest, sbuf, len) == 0)
			//		goto done;
			//	memcpy (sbuf, dest, len);
			//	if (! read_block (ctx, sbuf, 1, -1, write))	/* write */
			//		goto fail;
			//	goto done;
			//}
			grub_memmove64 (dest, (unsigned long long)(unsigned int)sbuf, len);
		}
	}
    }
done:
    ret = 1;
fail:
    disk_read_func = NULL;
    return ret;
}

static int read_attr(char* cur_mft,unsigned long long dest,unsigned long long ofs,unsigned long long len,int cached, unsigned long write)
{
  unsigned short save_cur;
  unsigned char attr;
  char* pp;
  int ret;

  save_cur=attr_cur;
  attr_nxt=attr_cur;
  attr=valueat(ofs2ptr(attr_nxt),0,unsigned char);
  if (get_aflag(AF_ALST))
    {
      unsigned short new_pos;
      unsigned long vcn;

      //vcn=ofs / (spc<<BLK_SHR);
      vcn=ofs >> log2_bpc;
      new_pos=attr_nxt+valueat(ofs2ptr(attr_nxt),4,unsigned short);
      while (new_pos<attr_end)
        {
          char *pa;

          pa=ofs2ptr(new_pos);
          if (*pa!=attr)
            break;
          if (valueat(pa,8,unsigned long)>vcn)
            break;
          attr_nxt=new_pos;
          new_pos+=valueat(pa,4,unsigned short);
        }
    }
  pp=find_attr(cur_mft,attr);
  ret=(pp)?read_data(cur_mft,pp,dest,ofs,len,cached,write):0;
  attr_cur=save_cur;
  return ret;
}

/*static*/ int read_mft(char* buf,unsigned long mftno)
{
  if (! read_attr(mmft,(unsigned long long)(unsigned int)buf,mftno*(mft_size << BLK_SHR),((unsigned long long)(mft_size)) << BLK_SHR,0, 0xedde0d90))
    {
      dbg_printf("Read MFT 0x%X fails\n",mftno);
      return 0;
    }
  return fixup(buf,mft_size,"FILE",0);
}

static int init_file(char* cur_mft,unsigned long mftno)
{
  unsigned short flag;

  if (! read_mft(cur_mft,mftno))
    goto error;

  flag=valueat(cur_mft,0x16,unsigned short);
  if ((flag & 1)==0)
    {
      dbg_printf("MFT 0x%X is not in use\n",mftno);
      goto error;
    }
  if (flag & 2)
    filemax=0;
  else
    {
      char *pa;

      pa=locate_attr(cur_mft,AT_DATA);
      if (pa==NULL)
        {
          dbg_printf("No $DATA in MFT 0x%X\n",mftno);
          goto error;
        }

      if (! pa[8])
        filemax=valueat(pa,0x10,unsigned long);
      else
        filemax=valueat(pa,0x30,unsigned long long);

      if (! get_aflag(AF_ALST))
        attr_end=0;		// Don't jump to attribute list
    }

  filepos=0;
  save_pos=1;
  return 1;
error:
  errnum=ERR_FSYS_CORRUPT;
  return 0;
}

static char ch;

static int list_file(char* cur_mft,char *fn,char *pos)
{
  char *np;
  unsigned char *utf8 = (unsigned char *)(NAME_BUF);
  unsigned long i,ns,len;

  //len=strlen(fn);
  for (len=strlen(fn); ! (pos[0xC] & 2); pos+=valueat(pos,8,unsigned short))
    {
      int is_print=(print_possibilities && ch != '/');
      if (pos[0xC] & 2)			// end signature
        break;
      np=pos+0x52;
      ns=valueat(np,-2,unsigned char);
      if (is_print && ns <= 12 && pos[0x51] == 2)		// exclude short names
         continue;
      ns=unicode_to_utf8((unsigned short *)np, utf8, ns);
      if (((is_print) && (ns>=len)) ||
          ((! is_print) && (ns==len)))
        {
          for (i=0;i<len;i++)
            if (tolower(((unsigned char*)fn)[i])!=tolower(utf8[i]/*np[i*2]*/))
              break;
          if (i>=len)
            {
              if (is_print)
                {
                  //if ((i) || ((utf8[0]!='$') && ((utf8[0]!='.') || (ns!=1))))
                    {
                      if (print_possibilities>0)
                        print_possibilities=-print_possibilities;
//                    for (i=1;i<ns;i++)
//                      np[i]=np[i*2];
//                    np[ns]=0;
#ifdef FS_UTIL
                      print_completion_ex(utf8,valueat(pos,0,unsigned long),valueat(pos,0x40,unsigned long),(valueat(pos,0x48,unsigned long) & ATTR_DIRECTORY)?FS_ATTR_DIRECTORY:0);
#else
											unsigned long long clo64 = current_color_64bit;
											unsigned long clo = current_color;
											if (valueat(pos,0x48,unsigned long) & ATTR_DIRECTORY)
											{
												if (current_term->setcolorstate)
													current_term->setcolorstate (COLOR_STATE_HIGHLIGHT);
												current_color_64bit = (current_color_64bit & 0xffffff) | (clo64 & 0xffffff00000000);
												current_color = (current_color & 0x0f) | (clo & 0xf0);
											}
                      print_a_completion((char *)utf8, 1);
											current_color_64bit = clo64;
											current_color = clo;
#endif
                    }
                }
              else
                {
                  if (valueat(pos,4,unsigned short))
                    {
                      dbg_printf("64-bit MFT number\n");
                      return 0;
                    }
                  return init_file(cur_mft,valueat(pos,0,unsigned long));
                }
            }
        }
      //pos+=valueat(pos,8,unsigned short);
    }
  return -1;
}

static int scan_dir(char* cur_mft,char *fn)
{
  unsigned char *bitmap;
  char *cur_pos;
  int bitmap_len,ret;

  if ((valueat(cur_mft,0x16,unsigned short) & 2)==0)
    {
      errnum=ERR_FILE_NOT_FOUND;
      return 0;
    }

  init_attr(cur_mft);
  while (1)
    {
      if ((cur_pos=find_attr(cur_mft,AT_INDEX_ROOT))==NULL)
        {
          dbg_printf("No $INDEX_ROOT\n");
          goto error;
        }

      // Resident, Namelen=4, Offset=0x18, Flags=0x00
      // Name="$I30"
      if ((valueat(cur_pos,8,unsigned long)!=0x180400) ||
          (valueat(cur_pos,0x18,unsigned long)!=0x490024) ||
          (valueat(cur_pos,0x1C,unsigned long)!=0x300033))
        continue;
      cur_pos+=valueat(cur_pos,0x14,unsigned short);
      if (*cur_pos!=0x30)	// Not filename index
        continue;
      break;
    }

  cur_pos+=0x10;		// Skip index root
  ret=list_file(cur_mft,fn,cur_pos+valueat(cur_pos,0,unsigned short));
  if (ret>=0)
    goto done;

  bitmap=NULL;
  bitmap_len=0;
  init_attr(cur_mft);
  while ((cur_pos=find_attr(cur_mft,AT_BITMAP))!=NULL)
    {
      int ofs=(unsigned char)cur_pos[0xA];
      // Namelen=4, Name="$I30"
      if ((cur_pos[9]==4) &&
          (valueat(cur_pos,ofs,unsigned long)==0x490024) &&
          (valueat(cur_pos,ofs+4,unsigned long)==0x300033))
        {
          if (cur_pos[8]==0)
            {
              bitmap_len=valueat(cur_pos,0x10,unsigned long);
              if (bitmap_len>4096)
                {
                  dbg_printf("Resident $BITMAP too large\n");
                  goto error;
                }
              bitmap=(unsigned char*)cbuf;
              memcpy((char *)bitmap,(char *)(cur_pos+valueat(cur_pos,0x14,unsigned short)),bitmap_len);
              break;
            }
          if (valueat(cur_pos,0x28,unsigned long)>4096)
            {
              dbg_printf("Non-resident $BITMAP too large\n");
              goto error;
            }
          bitmap=(unsigned char*)cbuf;
          bitmap_len=valueat(cur_pos,0x30,unsigned long);
          if (! read_data(cur_mft,cur_pos,(unsigned long long)(unsigned int)cbuf,0,valueat(cur_pos,0x28,unsigned long),0, 0xedde0d90))
            {
              dbg_printf("Fails to read non-resident $BITMAP\n");
              goto error;
            }
          break;
        }
    }

  cur_pos=locate_attr(cur_mft,AT_INDEX_ALLOCATION);
  while (cur_pos!=NULL)
    {
      // Non-resident, Namelen=4, Offset=0x40, Flags=0
      // Name="$I30"
      if ((valueat(cur_pos,8,unsigned long)==0x400401) &&
          (valueat(cur_pos,0x40,unsigned long)==0x490024) &&
          (valueat(cur_pos,0x44,unsigned long)==0x300033))
        break;
      cur_pos=find_attr(cur_mft,AT_INDEX_ALLOCATION);
    }

  if ((! cur_pos) && (bitmap))
    {
      dbg_printf("$BITMAP without $INDEX_ALLOCATION\n");
      goto error;
    }

  if (bitmap)
    {
      unsigned long v,i;

      v=1;
      for (i=0;i<bitmap_len*8;i++)
        {
          if (*bitmap & v)
            {
              if ((! read_attr(cur_mft,(unsigned long long)(unsigned int)sbuf,i*((unsigned long long)idx_size<<BLK_SHR),((unsigned long long)idx_size<<BLK_SHR),0, 0xedde0d90)) ||
                  (! fixup(sbuf,idx_size,"INDX",0)))
                goto error;
              ret=list_file(cur_mft,fn,&sbuf[0x18+valueat(sbuf,0x18,unsigned short)]);
              if (ret>=0)
                goto done;
            }
          v<<=1;
          if (v >= 0x100)
            {
              v=1;
              bitmap++;
            }
        }
    }

  ret=(print_possibilities<0);

done:
  if (! ret)
    errnum = ERR_FILE_NOT_FOUND;

  return ret;

error:
  errnum = ERR_FSYS_CORRUPT;
  return 0;
}

int ntfs_mount (void)
{
#if 0
  if (((current_drive & 0x80) || (current_slice != 0))
      && (current_slice != 7) && (current_slice != 0x17))
    return 0;
#endif

  if (! devread (0, 0, 512, (unsigned long long)(unsigned int)mmft, 0xedde0d90))
    return 0;

#if 0
  if (valueat(mmft,3,unsigned long)!=0x5346544E)
    return 0;
#endif

  blocksize=valueat(mmft,0xb,unsigned short);
  if (blocksize != 512)
    return 0;
  log2_bps = log2_tmp(blocksize);

  //spc=(blocksize*valueat(mmft,0xd,unsigned char)) >> BLK_SHR;
  spc = valueat(mmft,0xd,unsigned char) << (log2_bps - BLK_SHR);
  if (!spc || (128 % spc))
    return 0;
  log2_spc = log2_tmp(spc);
  log2_bpc = log2_spc + BLK_SHR;

  if (valueat(mmft,0x10,unsigned long) != 0)
    return 0;

  if (mmft[0x14] != 0)
    return 0;

  if (valueat(mmft,0x16,unsigned short) != 0)
    return 0;

  if ((unsigned short)(valueat(mmft,0x18,unsigned short) - 1) > 62)
    return 0;

  if ((unsigned short)(valueat(mmft,0x1A,unsigned short) - 1) > 255)
    return 0;

  if (valueat(mmft,0x20,unsigned long) != 0)
    return 0;

  if (mmft[0x44]>0)
    idx_size=spc*mmft[0x44];
  else
    idx_size=1<<(-mmft[0x44]-BLK_SHR);

  if (mmft[0x40]>0)
    mft_size=spc*mmft[0x40];
  else
    mft_size=1<<(-mmft[0x40]-BLK_SHR);

  mft_start=spc*valueat(mmft,0x30,unsigned long);

  if ((mft_size>MAX_MFT) ||(idx_size>MAX_IDX))
    return 0;

	*(unsigned long long *)0x3e7e00 = mft_start;
	*(unsigned long long *)0x3e7e08 = spc*valueat(mmft,0x38,unsigned long);
	
  if (! devread(mft_start,0,mft_size << BLK_SHR,(unsigned long long)(unsigned int)mmft, 0xedde0d90))
    return 0;

  if (! fixup(mmft,mft_size,"FILE",0))
    return 0;

  if (! locate_attr(mmft,AT_DATA))
    {
      dbg_printf("No $DATA in master MFT\n");
      return 0;
    }
  return 1;
}

int ntfs_dir (char *dirname)
{
  int ret;
//  int is_print=print_possibilities;

  filepos=filemax=0;

  if (*dirname=='/')
    dirname++;
  if ((*dirname=='#') && (dirname[1]>='0') && (dirname[1]<='9'))
    {
      unsigned long long mftno;

      dirname++;
      if (! safe_parse_maxint(&dirname,&mftno))
        return 0;
      return init_file(cmft,(unsigned long)mftno);
    }

  if (! init_file(cmft,FILE_ROOT))
    return 0;

  ret=0;

  while (1)
    {
      char *next/*, ch*/;

      /* skip to next slash or end of filename (space) */
      for (next = dirname; (ch = *next) && ch != '/' /*&& !isspace (ch)*/; next++)
      {
#if 0
	if (ch == '\\')
	{
		next++;
		if (! (ch = *next))
			break;
	}
#endif
      }

      *next = 0;
//      print_possibilities=(ch=='/')?0:is_print;

      ret=scan_dir(cmft,dirname);

//  print_possibilities=is_print;
      *next=ch;

      if (! ret)
        break;

      if (ch!='/')
        break;

      dirname=next+1;
    }

  return ret;
}

unsigned long long
ntfs_read(unsigned long long buf, unsigned long long len, unsigned long write)
{
  char *cur_mft;

  cur_mft=cmft;
  if (valueat(cur_mft,0x16,unsigned short) & 2)
    goto error;

  //if (disk_read_hook) /* commented out by chenall, 2010-05-13 */
    save_pos=1;

  if (! read_attr(cmft,buf,filepos,len,1,write))
    goto error;

  filepos+=len;
  return len;

error:
  errnum=ERR_FSYS_CORRUPT;
  return 0;
}

#ifdef FS_UTIL

void ntfs_info(int level)
{
  dbg_printf("blocksize: %u\nspc: %u\nmft_size: %u\nidx_size: %u\nmft_start: 0x%X\n",
             blocksize,spc,mft_size,idx_size,mft_start);
}

int ntfs_inode_read(char* buf)
{
  if (buf)
    memcpy(buf,cmft,mft_size<<BLK_SHR);
  return mft_size<<BLK_SHR;
}

static char* attr2str(unsigned char attr)
{
  switch (attr) {
  case AT_STANDARD_INFORMATION:
    return "$STANDARD_INFORMATION";
  case AT_ATTRIBUTE_LIST:
    return "$ATTRIBUTE_LIST";
  case AT_FILENAME:
    return "$FILENAME";
  case AT_OBJECT_ID:
    return "$OBJECT_ID";
  case AT_SECURITY_DESCRIPTOR:
    return "$SECURITY_DESCRIPTOR";
  case AT_VOLUME_NAME:
    return "$VOLUME_NAME";
  case AT_VOLUME_INFORMATION:
    return "$VOLUME_INFORMATION";
  case AT_DATA:
    return "$DATA";
  case AT_INDEX_ROOT:
    return "$INDEX_ROOT";
  case AT_INDEX_ALLOCATION:
    return "$INDEX_ALLOCATION";
  case AT_BITMAP:
    return "$BITMAP";
  case AT_SYMLINK:
    return "$SYMLINK";
  case AT_EA_INFORMATION:
    return "$EA_INFORMATION";
  case AT_EA:
    return "$EA";
  }
  return "$UNKNOWN";
}

static void print_name(char* s,int len)
{
  int i;

  for (i=0;i<len;i++)
    putchar((unsigned char)(s[i*2]), 255);
}

void print_runlist(char *run)
{
  read_ctx ctx;
  int first;

  memset(&ctx,0,sizeof(ctx));
  first=1;
  while (run=read_run_list(&ctx,run))
    {
      if (first)
        first=0;
      else
        putchar(',', 255);
      if (ctx.flags & RF_BLNK)
        printf("(+%d)",((ctx.next_vcn-ctx.curr_vcn)*spc));
      else
        printf("%d+%d",(ctx.curr_lcn*spc),((ctx.next_vcn-ctx.curr_vcn)*spc));
      if (*run==0)
        break;
    }
  printf("\n");
}

void ntfs_inode_info(int level)
{
  char *cur_mft,*pos;
  int first;

  cur_mft=cmft;
  printf("Type: %s\n",((valueat(cur_mft,0x16,unsigned short) & 2)?"Directory":"File"));
  if (valueat(cur_mft,0x20,unsigned long))
    printf("Base: 0x%X\n",(valueat(cur_mft,0x20,unsigned long)));
  printf("Attr:\n");

  first=1;
  init_attr(cur_mft);
  while ((pos=find_attr(cur_mft,0))!=NULL)
    {
      unsigned long fg;

      if (get_aflag(AF_ALST))
        {
          if (first)
            {
              printf("Attr List:\n");
              first=0;
            }
        }
      printf("  %s (0x%X) ",attr2str(*pos),(unsigned long)(unsigned char)*pos);

      printf((pos[8])?"(nr":"(r");

      fg=valueat(pos,0xC,unsigned short);
      if (fg & FLAG_COMPRESSED)
        printf(",c");
      if (fg & FLAG_ENCRYPTED)
        printf(",e");
      if (fg & FLAG_SPARSE)
        printf(",s");

      if (get_aflag(AF_ALST))
        {
          printf(",mft=0x%X",(valueat(ofs2ptr(attr_cur),0x10,unsigned long)));
          if (pos[8])
            printf(",vcn=0x%X",(valueat(ofs2ptr(attr_cur),0x8,unsigned long)));
        }

      if (pos[9])
        {
          printf(",nm=");
          print_name(pos+valueat(pos,0xA,unsigned short),pos[9]);
        }

      printf(",sz=%d",(valueat(pos,((pos[8])?0x30:0x10),unsigned long)));

      printf(")\n");
      if ((pos[8]) && (! get_aflag(AF_ALST)))
        {
          printf("    ");
          print_runlist(pos+valueat(pos,0x20,unsigned short));
        }
      switch ((unsigned char)pos[0]) {
      case AT_FILENAME:
        pos+=valueat(pos,0x14,unsigned short);
        if (pos[0x40])
          {
            printf("    ");
            print_name(pos+0x42,(unsigned char)pos[0x40]);
            printf("\n");
          }
        break;
      }
    }
}

#endif

#endif /* FSYS_NTFS */
