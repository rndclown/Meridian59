// Meridian 59, Copyright 1994-2012 Andrew Kirmse and Chris Kirmse.
// All rights reserved.
//
// This software is distributed under a license that is described in
// the LICENSE file that accompanies it.
//
// Meridian is a registered trademark.
/*
* loadbof.c
*

  This module loads the compiled kod (.bof files) into memory, as memory
  mapped files.  A linked list of structures with the stuff about the
  file is maintained.  When each .bof file is loaded, the classes and
  message handlers are created by class.c and message.c.  The format of
  the .bof files is in bof.txt.
  
*/

#include "blakserv.h"

#define BOF_SPEC "*.bof"

/* I don't check the magic # yet, but here's what it should be */
static unsigned char magic_num[] = { 'B', 'O', 'F', 0xFF };
#define BOF_MAGIC_LEN sizeof(magic_num)


/* variables */
loaded_bof_node *mem_files;

/* local function prototypes */
Bool LoadBofName(char *fname);
void AddFileMem(char *fname,char *ptr,int size);
void FindClasses(char *fmem,char *fname);
void FindMessages(char *fmem,int class_id,bof_dispatch *dispatch);

void InitLoadBof(void)
{
	mem_files = NULL;
}

void LoadBof(void)
{
	HANDLE hFindFile;
	WIN32_FIND_DATA search_data;
	int files_found;
	int files_loaded;
	char file_load_path[MAX_PATH+FILENAME_MAX];
	char file_copy_path[MAX_PATH+FILENAME_MAX];
	
	files_found = 0;
	files_loaded = 0;
	
	sprintf(file_load_path,"%s%s",ConfigStr(PATH_BOF),BOF_SPEC);
	hFindFile = FindFirstFile(file_load_path,&search_data);
	if (hFindFile != INVALID_HANDLE_VALUE)
	{
		do
		{
			files_found++; 
			sprintf(file_load_path,"%s%s",ConfigStr(PATH_BOF),search_data.cFileName);
			sprintf(file_copy_path,"%s%s",ConfigStr(PATH_MEMMAP),search_data.cFileName);
			if (BlakMoveFile(file_load_path,file_copy_path))
				files_loaded++;
		} while (FindNextFile(hFindFile,&search_data));
		FindClose(hFindFile);
	}
	
	/*
	if (files_found > 0)
	dprintf("LoadBof moved in %i of %i found new .bof files\n",files_loaded,files_found);
	*/

	//dprintf("starting to load bof files\n");
	files_found = 0;
	files_loaded = 0;
	
	sprintf(file_load_path,"%s%s",ConfigStr(PATH_MEMMAP),BOF_SPEC);
	hFindFile = FindFirstFile(file_load_path,&search_data);
	if (hFindFile != INVALID_HANDLE_VALUE)
	{
		do
		{
			files_found++; 
			sprintf(file_load_path,"%s%s",ConfigStr(PATH_MEMMAP),search_data.cFileName);
			
			if (LoadBofName(file_load_path))
				files_loaded++;
			else
				eprintf("LoadAllBofs can't load %s\n",search_data.cFileName);
		} while (FindNextFile(hFindFile,&search_data));
		FindClose(hFindFile);
	}
	
	SetClassesSuperPtr();
	SetClassVariables();
	SetMessagesPropagate();

	//dprintf("LoadBof loaded %i of %i found .bof files\n",files_loaded,files_found);
}

void ResetLoadBof(void)
{ 
	loaded_bof_node *lf,*temp;
	
	lf = mem_files;
	while (lf != NULL)
	{
		temp = lf->next;
		
		FreeMemory(MALLOC_ID_LOADBOF,lf->mem,lf->length);
		
		FreeMemory(MALLOC_ID_LOADBOF,lf,sizeof(loaded_bof_node));
		lf = temp;
	}
	mem_files = NULL;
}

Bool LoadBofName(char *fname)
{
	HANDLE fh,mapfh;
	char *file_mem;
	int file_size;
	char *ptr;

	fh = CreateFile(fname,GENERIC_READ,FILE_SHARE_READ,NULL,
		OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
	if (fh == INVALID_HANDLE_VALUE)
	{
		return False;
	}
	
	file_size = GetFileSize(fh,NULL);
	
	mapfh = CreateFileMapping(fh,NULL,PAGE_READONLY,0,file_size,NULL);
	if (mapfh == NULL)
	{
		CloseHandle(fh);
		return False;
	}
	
	file_mem = (char *)MapViewOfFile(mapfh,FILE_MAP_READ,0,0,0);
	if (file_mem == NULL)
	{
		CloseHandle(mapfh);
		CloseHandle(fh);
		return False;
	}
	
	if (((bof_file_header *)file_mem)->version != 5)
	{
		eprintf("LoadBofName %s can't understand bof version != 5\n",fname);
		UnmapViewOfFile(file_mem);
		CloseHandle(mapfh);
		CloseHandle(fh);
		return True;
	}

	ptr = (char *)AllocateMemory(MALLOC_ID_LOADBOF,file_size);
	memcpy(ptr,file_mem,file_size);

	UnmapViewOfFile(file_mem);
	CloseHandle(mapfh);
	CloseHandle(fh);

	AddFileMem(fname,ptr,file_size);
	
	return True;
}

/* add a filename and mapped ptr to the list of loaded files */
void AddFileMem(char *fname,char *ptr,int size)
{
	loaded_bof_node *lf;
	
	/* make new loaded_file node */
	lf = (loaded_bof_node *)AllocateMemory(MALLOC_ID_LOADBOF,sizeof(loaded_bof_node));
	strcpy(lf->fname,fname);
	lf->mem = ptr;
	lf->length = size;
	
	/* we store the fname so the class structures can point to it, but kill the path */
	
	if (strrchr(lf->fname,'\\') == NULL)
		FindClasses(lf->mem,lf->fname); 
	else
		FindClasses(lf->mem,strrchr(lf->fname,'\\')+1); 
	
	/* add to front of list */
	lf->next = mem_files;
	mem_files = lf;
}

/* add the classes of a mapped file to a class list */
void FindClasses(char *fmem,char *fname)
{
	int dstring_offset;
	int classes_in_file,class_id,class_offset;
	bof_class_header *class_data;
	bof_dispatch *dispatch_section;
	bof_dstring *dstring_data;
	int line_table_offset;
	bof_line_table *line_table;
	bof_class_props *props;
   bof_list_elem *classes;
	int i;
	
	dstring_offset = ((bof_file_header *)fmem)->dstring_offset;
	dstring_data = (bof_dstring *)(fmem + dstring_offset);
	line_table_offset = ((bof_file_header *)fmem)->line_table_offset;
	if (line_table_offset == 0) /* means no line number table */
		line_table = NULL;
	else
		line_table = (bof_line_table *)(fmem + line_table_offset);
	
	classes_in_file = ((bof_file_header *)fmem)->num_classes;
   
   classes = &((bof_file_header *)fmem)->classes;
	for (i=0;i<classes_in_file;i++)
	{
		class_id     = classes[i].id;
		class_offset = classes[i].offset;
		class_data = (bof_class_header *)(fmem + class_offset);
		
		props = (bof_class_props *)(fmem + class_data->offset_properties);
		
		dispatch_section = (bof_dispatch *)(fmem + class_data->offset_dispatch);
		AddClass(class_id,class_data,fname,fmem,dstring_data,line_table,props);
		SetClassNumMessages(class_id,dispatch_section->num_messages);
		FindMessages(fmem,class_id,dispatch_section);
	}
}

void FindMessages(char *fmem,int class_id,bof_dispatch *dispatch)
{
	int i;
   bof_dispatch_list_elem *messages;

   messages = &dispatch->messages;
   
	for (i=0;i<dispatch->num_messages;i++)
		AddMessage(class_id,i,messages[i].id,
                 (char *)(fmem + messages[i].offset),
                 messages[i].dstr_id);
}
