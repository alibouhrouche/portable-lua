/*
** $Id: lua.c,v 1.160.1.2 2007/12/28 15:32:23 roberto Exp $
** Lua stand-alone interpreter
** See Copyright Notice in lua.h
*/


#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <winapi/windows.h>
#include "MemoryModule.h"
#include <io.h>
#include <sys/stat.h>
#include <stdint.h>

#define IS_BIG_ENDIAN (*(uint16_t *)"\0\xff" < 0x100)

#define __bswap_constant_32(x) \
          ((((x) & 0xff000000) >> 24) | (((x) & 0x00ff0000) >>  8) | \
           (((x) & 0x0000ff00) <<  8) | (((x) & 0x000000ff) << 24))

#define lua_c

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"

typedef BYTE SIGNATURE[64];

const SIGNATURE luasig = {0xAB,0x41,0x6C,0x69,0x00,0x00,0x00,0x00,
                          0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                          0x62,0x38,0x71,0xA0,0xEE,0x66,0xD4,0x47,
                          0x81,0x4F,0xA5,0x00,0xAA,0xFE,0x74,0x0B,
                          0x71,0xCC,0x8F,0x4F,0xDB,0xB0,0x0F,0x40,
                          0xA2,0x1B,0x0E,0x5C,0x00,0xB2,0x39,0xA4,
                          0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                          0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01};

static lua_State *globalL = NULL;

static const char *progname = LUA_PROGNAME;

extern int luaopen_bit(lua_State *L);

typedef struct{
    unsigned long nofsec;
    BYTE conf[4];
} CORE_HEADER;

typedef struct{
	unsigned char name[59];
    BYTE Characteristics;
	unsigned int size;
} SECTION_HEADER;

typedef struct{
	FILE *f;
	uint8_t _io;
	CORE_HEADER chead;
	unsigned int Coffset;
	SECTION_HEADER *heads;
	unsigned int *offsets;
	uint8_t *wtype;
	unsigned int *pos;
    union{
        char *filename;
        uint8_t *data;
	} *data;
} CORE_HANDLE;

typedef struct{
	CORE_HANDLE *chandle;
	unsigned int secnum;
} SECTION_HANDLE;

CORE_HANDLE *g_ch;
CORE_HANDLE *C_ch;

typedef struct
{
 FILE *f;
 size_t size;
 char buff[512];
} State;

static const char *myget(lua_State *L, void *data, size_t *size)
{
 State* s=data;
 size_t n;
 (void)L;
 n=(sizeof(s->buff)<=s->size)? sizeof(s->buff) : s->size;
 n=fread(s->buff,1,n,s->f);
 s->size-=n;
 *size=n;
 return (n>0) ? s->buff : NULL;
}

#define cannot(x) luaL_error(L,"cannot %s %s: %s",x,name,strerror(errno))

static void load(lua_State *L,FILE *f, const char *name, unsigned int offset, unsigned int size)
{
 State S;
 int c;
 if (fseek(f,offset,SEEK_SET)!=0) cannot("seek");
 S.f=f; S.size=size;
 c=getc(f);
 if (c=='#')
  while (--S.size>0 && c!='\n') c=getc(f);
 else
  ungetc(c,f);
 if (lua_load(L,myget,&S,"=")!=0) lua_error(L);
}


static void lstop (lua_State *L, lua_Debug *ar) {
  (void)ar;  /* unused arg. */
  lua_sethook(L, NULL, 0, 0);
  luaL_error(L, "interrupted!");
}


static void laction (int i) {
  signal(i, SIG_DFL); /* if another SIGINT happens before lstop,
                              terminate process (default action) */
  lua_sethook(globalL, lstop, LUA_MASKCALL | LUA_MASKRET | LUA_MASKCOUNT, 1);
}


static void print_usage (void) {
  fprintf(stderr,
  "usage: %s [options] [script [args]].\n"
  "Available options are:\n"
  "  -e stat  execute string " LUA_QL("stat") "\n"
  "  -l name  require library " LUA_QL("name") "\n"
  "  -i       enter interactive mode after executing " LUA_QL("script") "\n"
  "  -v       show version information\n"
  "  --       stop handling options\n"
  "  -        execute stdin and stop handling options\n"
  ,
  progname);
  fflush(stderr);
}


static void l_message (const char *pname, const char *msg) {
  if (pname) fprintf(stderr, "%s: ", pname);
  fprintf(stderr, "%s\n", msg);
  fflush(stderr);
}


static int report (lua_State *L, int status) {
  if (status && !lua_isnil(L, -1)) {
    const char *msg = lua_tostring(L, -1);
    if (msg == NULL) msg = "(error object is not a string)";
    l_message(progname, msg);
    lua_pop(L, 1);
  }
  return status;
}


static int traceback (lua_State *L) {
  if (!lua_isstring(L, 1))  /* 'message' not a string? */
    return 1;  /* keep it intact */
  lua_getfield(L, LUA_GLOBALSINDEX, "debug");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return 1;
  }
  lua_getfield(L, -1, "traceback");
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 2);
    return 1;
  }
  lua_pushvalue(L, 1);  /* pass error message */
  lua_pushinteger(L, 2);  /* skip this function and traceback */
  lua_call(L, 2, 1);  /* call debug.traceback */
  return 1;
}


static int docall (lua_State *L, int narg, int clear) {
  int status;
  int base = lua_gettop(L) - narg;  /* function index */
  lua_pushcfunction(L, traceback);  /* push traceback function */
  lua_insert(L, base);  /* put it under chunk and args */
  signal(SIGINT, laction);
  status = lua_pcall(L, narg, (clear ? 0 : LUA_MULTRET), base);
  signal(SIGINT, SIG_DFL);
  lua_remove(L, base);  /* remove traceback function */
  /* force a complete garbage collection in case of errors */
  if (status != 0) lua_gc(L, LUA_GCCOLLECT, 0);
  return status;
}


static void print_version (void) {
  l_message(NULL, LUA_RELEASE "  " LUA_COPYRIGHT);
}


static int getargs (lua_State *L, char **argv, int n) {
  int narg;
  int i;
  int argc = 0;
  while (argv[argc]) argc++;  /* count total number of arguments */
  narg = argc - (n + 1);  /* number of arguments to the script */
  luaL_checkstack(L, narg + 3, "too many arguments to script");
  for (i=n+1; i < argc; i++)
    lua_pushstring(L, argv[i]);
  lua_createtable(L, narg, n + 1);
  for (i=0; i < argc; i++) {
    lua_pushstring(L, argv[i]);
    lua_rawseti(L, -2, i - n);
  }
  return narg;
}


static int dofile (lua_State *L, const char *name) {
  int status = luaL_loadfile(L, name) || docall(L, 0, 1);
  return report(L, status);
}


static int dostring (lua_State *L, const char *s, const char *name) {
  int status = luaL_loadbuffer(L, s, strlen(s), name) || docall(L, 0, 1);
  return report(L, status);
}


static int dolibrary (lua_State *L, const char *name) {
  lua_getglobal(L, "require");
  lua_pushstring(L, name);
  return report(L, docall(L, 1, 1));
}


static const char *get_prompt (lua_State *L, int firstline) {
  const char *p;
  lua_getfield(L, LUA_GLOBALSINDEX, firstline ? "_PROMPT" : "_PROMPT2");
  p = lua_tostring(L, -1);
  if (p == NULL) p = (firstline ? LUA_PROMPT : LUA_PROMPT2);
  lua_pop(L, 1);  /* remove global */
  return p;
}


static int incomplete (lua_State *L, int status) {
  if (status == LUA_ERRSYNTAX) {
    size_t lmsg;
    const char *msg = lua_tolstring(L, -1, &lmsg);
    const char *tp = msg + lmsg - (sizeof(LUA_QL("<eof>")) - 1);
    if (strstr(msg, LUA_QL("<eof>")) == tp) {
      lua_pop(L, 1);
      return 1;
    }
  }
  return 0;  /* else... */
}


static int pushline (lua_State *L, int firstline) {
  char buffer[LUA_MAXINPUT];
  char *b = buffer;
  size_t l;
  const char *prmt = get_prompt(L, firstline);
  if (lua_readline(L, b, prmt) == 0)
    return 0;  /* no input */
  l = strlen(b);
  if (l > 0 && b[l-1] == '\n')  /* line ends with newline? */
    b[l-1] = '\0';  /* remove it */
  if (firstline && b[0] == '=')  /* first line starts with `=' ? */
    lua_pushfstring(L, "return %s", b+1);  /* change it to `return' */
  else
    lua_pushstring(L, b);
  lua_freeline(L, b);
  return 1;
}


static int loadline (lua_State *L) {
  int status;
  lua_settop(L, 0);
  if (!pushline(L, 1))
    return -1;  /* no input */
  for (;;) {  /* repeat until gets a complete line */
    status = luaL_loadbuffer(L, lua_tostring(L, 1), lua_strlen(L, 1), "=stdin");
    if (!incomplete(L, status)) break;  /* cannot try to add lines? */
    if (!pushline(L, 0))  /* no more input? */
      return -1;
    lua_pushliteral(L, "\n");  /* add a new line... */
    lua_insert(L, -2);  /* ...between the two lines */
    lua_concat(L, 3);  /* join them */
  }
  lua_saveline(L, 1);
  lua_remove(L, 1);  /* remove line */
  return status;
}


static void dotty (lua_State *L) {
  int status;
  const char *oldprogname = progname;
  progname = NULL;
  while ((status = loadline(L)) != -1) {
    if (status == 0) status = docall(L, 0, 0);
    report(L, status);
    if (status == 0 && lua_gettop(L) > 0) {  /* any result to print? */
      lua_getglobal(L, "print");
      lua_insert(L, 1);
      if (lua_pcall(L, lua_gettop(L)-1, 0, 0) != 0)
        l_message(progname, lua_pushfstring(L,
                               "error calling " LUA_QL("print") " (%s)",
                               lua_tostring(L, -1)));
    }
  }
  lua_settop(L, 0);  /* clear stack */
  fputs("\n", stdout);
  fflush(stdout);
  progname = oldprogname;
}


static int handle_script (lua_State *L, char **argv, int n) {
  int status;
  const char *fname;
  int narg = getargs(L, argv, n);  /* collect arguments */
  lua_setglobal(L, "arg");
  fname = argv[n];
  if (strcmp(fname, "-") == 0 && strcmp(argv[n-1], "--") != 0)
    fname = NULL;  /* stdin */
  status = luaL_loadfile(L, fname);
  lua_insert(L, -(narg+1));
  if (status == 0)
    status = docall(L, narg, 0);
  else
    lua_pop(L, narg);
  return report(L, status);
}


/* check that argument has no extra characters at the end */
#define notail(x)	{if ((x)[2] != '\0') return -1;}


static int collectargs (char **argv, int *pi, int *pv, int *pe) {
  int i;
  for (i = 1; argv[i] != NULL; i++) {
    if (argv[i][0] != '-')  /* not an option? */
        return i;
    switch (argv[i][1]) {  /* option */
      case '-':
        notail(argv[i]);
        return (argv[i+1] != NULL ? i+1 : 0);
      case '\0':
        return i;
      case 'i':
        notail(argv[i]);
        *pi = 1;  /* go through */
      case 'v':
        notail(argv[i]);
        *pv = 1;
        break;
      case 'e':
        *pe = 1;  /* go through */
      case 'l':
        if (argv[i][2] == '\0') {
          i++;
          if (argv[i] == NULL) return -1;
        }
        break;
      default: return -1;  /* invalid option */
    }
  }
  return 0;
}


static int runargs (lua_State *L, char **argv, int n) {
  int i;
  for (i = 1; i < n; i++) {
    if (argv[i] == NULL) continue;
    lua_assert(argv[i][0] == '-');
    switch (argv[i][1]) {  /* option */
      case 'e': {
        const char *chunk = argv[i] + 2;
        if (*chunk == '\0') chunk = argv[++i];
        lua_assert(chunk != NULL);
        if (dostring(L, chunk, "=(command line)") != 0)
          return 1;
        break;
      }
      case 'l': {
        const char *filename = argv[i] + 2;
        if (*filename == '\0') filename = argv[++i];
        lua_assert(filename != NULL);
        if (dolibrary(L, filename))
          return 1;  /* stop if file fails */
        break;
      }
      default: break;
    }
  }
  return 0;
}


static int handle_luainit (lua_State *L) {
  const char *init = getenv(LUA_INIT);
  if (init == NULL) return 0;  /* status OK */
  else if (init[0] == '@')
    return dofile(L, init+1);
  else
    return dostring(L, init, "=" LUA_INIT);
}


struct Smain {
  int argc;
  char **argv;
  int status;
};

int sizeofpe(FILE *f){
    int pos = ftell(f);
    uint32_t pointdata = 0;
    uint32_t sizepd = 0;
    IMAGE_DOS_HEADER doshead;
    rewind(f);
    fread(&doshead,sizeof(IMAGE_DOS_HEADER),1,f);
    fseek(f,doshead.e_lfanew,SEEK_SET);
    IMAGE_NT_HEADERS ntheaders;
    fread(&ntheaders,sizeof(IMAGE_NT_HEADERS),1,f);
    for(int i=0;i<ntheaders.FileHeader.NumberOfSections;i++){
        IMAGE_SECTION_HEADER sect;
        fread(&sect,sizeof(IMAGE_SECTION_HEADER),1,f);
        if(sect.PointerToRawData > pointdata){
            pointdata = sect.PointerToRawData;
            sizepd = sect.SizeOfRawData;
        }
    }
    fseek(f,pos,SEEK_SET);
    return pointdata+sizepd;
}

int pesize;
int filesize;
char* getprog(void) {
  int nsize = 4096 + 1;
  char* progdir = malloc(nsize * sizeof(char));
  char *lb;
  int n;
  n = GetModuleFileNameA(NULL, progdir, nsize);
  if (n == 0 || n == nsize || (lb = strrchr(progdir, (int)LUA_DIRSEP[0])) == NULL)
    return(NULL);
  return(progdir);
}

typedef struct{
    bool isfmemmod;
}MMODULE,*PMMODULE;

static HCUSTOMMODULE _LoadLibraryLua(LPCSTR filename, void *userdata);
static FARPROC _GetProcAddressLua(HCUSTOMMODULE module, LPCSTR name, void *userdata);
static void _FreeLibraryLua(HCUSTOMMODULE module, void *userdata);

static HCUSTOMMODULE _LoadLibraryLua(LPCSTR filename, void *userdata)
{
    PMMODULE hmm = (PMMODULE)userdata;
    hmm->isfmemmod = 0;
    if((strcmp(filename,"lua51.dll") == 0)||(strcmp(filename,"lua5.1.dll") == 0)){return (HCUSTOMMODULE) GetModuleHandle(0);}
    for (unsigned int i = 0;i<C_ch->chead.nofsec;i++){
        if(strcmp(C_ch->heads[i].name,filename) == 0){
            if((C_ch->heads[i].Characteristics & 0x08) == 0x08){
                    uint8_t *data = malloc(C_ch->heads[i].size);
                    fseek(C_ch->f,C_ch->offsets[i],SEEK_SET);
                    fread(data,C_ch->heads[i].size,1,C_ch->f);
                    PMMODULE mm = malloc(sizeof(MMODULE));
                    mm->isfmemmod = 0;
                    hmm->isfmemmod = 1;
                    return (HCUSTOMMODULE) MemoryLoadLibraryEx(data,_LoadLibraryLua,_GetProcAddressLua,_FreeLibraryLua,mm);
            }
        }
    }
    HMODULE result = LoadLibraryA(filename);
    if (result == NULL) {
        return NULL;
    }

    return (HCUSTOMMODULE) result;
}

static FARPROC _GetProcAddressLua(HCUSTOMMODULE module, LPCSTR name, void *userdata)
{
    PMMODULE mm = (PMMODULE)userdata;
    if(mm->isfmemmod == 1) {
        return (FARPROC) MemoryGetProcAddress(module,name);
    }else{
        return (FARPROC) GetProcAddress((HMODULE) module, name);
    }
}

static void _FreeLibraryLua(HCUSTOMMODULE module, void *userdata)
{
    PMMODULE mm = (PMMODULE)userdata;
    if(mm->isfmemmod){
        MemoryFreeLibrary(module);
    }else{
        if(module != GetModuleHandle(0)){
            FreeLibrary((HMODULE) module);
        }
    }
}

/* prefix for open functions in C libraries */
#define LUA_POF		"luaopen_"

/* separator for open functions in C libraries */
#define LUA_OFSEP	"_"

#define POF		LUA_POF

static const char *mkfuncname (lua_State *L, const char *modname) {
  const char *funcname;
  const char *mark = strchr(modname, *LUA_IGMARK);
  if (mark) modname = mark + 1;
  funcname = luaL_gsub(L, modname, ".", LUA_OFSEP);
  funcname = lua_pushfstring(L, POF"%s", funcname);
  lua_remove(L, -2);  /* remove 'gsub' result */
  return funcname;
}

#define LIBPREFIX	"LOADLIB: "

static void **ll_register (lua_State *L, const char *path) {
  void **plib;
  lua_pushfstring(L, "%s%s", LIBPREFIX, path);
  lua_gettable(L, LUA_REGISTRYINDEX);  /* check library in registry? */
  if (!lua_isnil(L, -1))  /* is there an entry? */
    plib = (void **)lua_touserdata(L, -1);
  else {  /* no entry yet; create one */
    lua_pop(L, 1);
    plib = (void **)lua_newuserdata(L, sizeof(const void *));
    *plib = NULL;
    luaL_getmetatable(L, "_LOADLIB");
    lua_setmetatable(L, -2);
    lua_pushfstring(L, "%s%s", LIBPREFIX, path);
    lua_pushvalue(L, -2);
    lua_settable(L, LUA_REGISTRYINDEX);
  }
  return plib;
}

int llib(lua_State* state,const char *name,const char *init,unsigned int secn){
    unsigned int n = secn;
    if(C_ch != NULL){
        if((C_ch->chead.conf[3] & 0x08) == 0x08){
            if(name != NULL){
                    for (unsigned int i = 0;i<C_ch->chead.nofsec;i++){
                        if((strcmp(C_ch->heads[i].name,name) == 0)&&((C_ch->heads[i].Characteristics & 0x04) == 0x04)){
                           n = i;
                        }
                    }
            }
            unsigned char* data = malloc(C_ch->heads[n].size); //Allocating Memory for DLL Data
            fseek(C_ch->f,C_ch->offsets[n],SEEK_SET); //Seeking To DLL Address
            fread(data,C_ch->heads[n].size,1,C_ch->f); //Reading To Memory
            PMMODULE mm = malloc(sizeof(MMODULE)); //Allocating Memory For UserData
            mm->isfmemmod = 0; //Set It To Default
            void **reg = ll_register(state, name); //Registering C Function
            if (*reg == NULL) *reg = MemoryLoadLibraryEx(data,_LoadLibraryLua,_GetProcAddressLua,_FreeLibraryLua,mm); //Load DLL From Memory
            if (*reg == NULL) //Check if Loaded
                return 0;
            else {
            lua_CFunction f = MemoryGetProcAddress(*reg,init); //Get Address For Function
            if (f == NULL)
              return 0;  /* unable to find function */
            lua_pushcfunction(state, f); //Push C Function Back To Lua
            }
            return 1;
        }
    }
    return 0;
}

int MyLoader(lua_State* state) {
    if(C_ch != NULL){
        const char *name = luaL_checkstring(state, 1);
        for (unsigned int i = 0;i<C_ch->chead.nofsec;i++){
            if(strcmp(C_ch->heads[i].name,name) == 0){
                if((C_ch->heads[i].Characteristics & 0x02) == 0x02){
                    load(state,C_ch->f,name,C_ch->offsets[i],C_ch->heads[i].size);
                    return 1;
                }else if((C_ch->heads[i].Characteristics & 0x04) == 0x04){
                    const char *funcname;
                    funcname = mkfuncname(state,name);
                    llib(state,name,funcname,i);
                    return 1;
                }
            }
        }
    }
	lua_pushstring(state,"Not Found");
    return 0;
}



#define LUA_FPLUAHANDLE		"FPLUA*"

#define tochp(L)	((CORE_HANDLE **)luaL_checkudata(L, 1, LUA_FPLUAHANDLE))
#define closed(L)    luaL_error(L,"attempt to use a closed file")

int checkString( const char s[] )
{
    unsigned char c;

    while ( ( c = *s ) && ( isprint( c ) ) ) ++s;

    return *s == '\0';
}

/*static CORE_HANDLE *check_ch(lua_State *L, int index){
    CORE_HANDLE *ch;
    luaL_checktype(L,index,LUA_TUSERDATA);
    ch = (CORE_HANDLE *)luaL_checkudata(L,index,LUA_FPLUAHANDLE);
    if(ch == NULL) luaL_typerror(L, index, LUA_FPLUAHANDLE);
    return ch;
}*/

static int plua_read(lua_State *L){
    if(g_ch != NULL){
    unsigned int n = luaL_checknumber(L, 1);
    if(n > 0){
        if(n < g_ch->chead.nofsec+1){
            unsigned char *data = malloc(g_ch->heads[n-1].size);
            fseek(g_ch->f,g_ch->offsets[n-1],SEEK_SET);
            fread(data,g_ch->heads[n-1].size,1,g_ch->f);
            lua_pushlstring(L,data,g_ch->heads[n-1].size);
            free(data);
            return 1;
        }
    }
    }
    return 0;
}

static int plua_getn(lua_State *L){
    if(g_ch != NULL){
    const char *name = luaL_checkstring(L, 1);
    for (unsigned int i = 0;i<g_ch->chead.nofsec;i++){
        if(strcmp(g_ch->heads[i].name,name) == 0){
            lua_pushinteger(L,i+1);
            return 1;
        }
	}
    }
	return 0;
}

static int plua_list(lua_State *L){
    if(g_ch != NULL){
    lua_newtable(L);
    for (unsigned int i = 0;i<g_ch->chead.nofsec;i++){
        lua_newtable(L);
        lua_pushstring(L,g_ch->heads[i].name);
        lua_rawseti(L,-2,1);
        lua_pushinteger(L,g_ch->heads[i].size);
        lua_rawseti(L,-2,2);
        lua_pushinteger(L,g_ch->heads[i].Characteristics);
        lua_rawseti(L,-2,3);
        lua_rawseti(L,-2,i+1);
    }
    }
    return 1;
}

CORE_HANDLE* pluaload(const char* fname){
    CORE_HANDLE *ch = malloc(sizeof(CORE_HANDLE));
    FILE *f = fopen(fname,"r+b");
    if(f == NULL) return NULL;
    ch->f = f;
    int currpos;
    ch->_io = 0;
    SIGNATURE sig;
    fread(sig,sizeof(SIGNATURE),1,f);
    ch->Coffset = sizeof(SIGNATURE);
    if(memcmp(&luasig,&sig,sizeof(SIGNATURE)) == 0){
        fread(&ch->chead,sizeof(CORE_HEADER),1,f);
        ch->heads = malloc(sizeof(SECTION_HEADER)*ch->chead.nofsec);
        ch->offsets = malloc(sizeof(unsigned int)*(ch->chead.nofsec+1));
        fread(ch->heads,sizeof(SECTION_HEADER),ch->chead.nofsec,f);
        ch->pos = malloc(sizeof(unsigned int)*ch->chead.nofsec+1);
        currpos = ch->Coffset+sizeof(CORE_HEADER)+sizeof(SECTION_HEADER)*ch->chead.nofsec;
        for(unsigned int i = 0; i < ch->chead.nofsec; i++){
             ch->heads[i].name[58] = '\0';
             if(!checkString(ch->heads[i].name)){return 0;}
             if(((ch->heads[i].Characteristics & 0x10) == 0x10)&&(i != 0)){
                ch->offsets[i] = ch->offsets[i-1];
                ch->heads[i].size = ch->heads[i-1].size;
                ch->heads[i].Characteristics = ch->heads[i-1].Characteristics;
             }else{
             ch->offsets[i] = currpos;
             currpos += ch->heads[i].size;
             }
             ch->pos[i] = 0;
         }
         ch->offsets[ch->chead.nofsec] = currpos;
         ch->pos[ch->chead.nofsec] = 0;
    }else{
        return NULL;
    }
    return ch;
}

static int plua_load(lua_State *L){
    CORE_HANDLE **ch = (CORE_HANDLE **)lua_newuserdata(L,sizeof(CORE_HANDLE*));
    luaL_getmetatable(L, LUA_FPLUAHANDLE);
    lua_setmetatable(L, -2);
    const char* fname = lua_tostring(L,1);
    (*ch) = pluaload(fname);
    return (*ch == NULL) ? 0 : 1;
}

static int plua_new(lua_State *L){
    unsigned int n = luaL_checknumber(L, 1);
    CORE_HANDLE **ch = (CORE_HANDLE **)lua_newuserdata(L,sizeof(CORE_HANDLE*));
    *ch = malloc(sizeof(CORE_HANDLE));
    luaL_getmetatable(L, LUA_FPLUAHANDLE);
    lua_setmetatable(L, -2);
    (*ch)->_io = 1;
    (*ch)->chead.nofsec = n;
    memset((*ch)->chead.conf,0,sizeof(int));
    (*ch)->wtype = malloc(sizeof(uint8_t)*n+1);
    memset((*ch)->wtype,0,sizeof(uint8_t)*n+1);
    (*ch)->data = malloc(sizeof(void*)*n+1);
    (*ch)->heads = malloc(sizeof(SECTION_HEADER)*n+1);
    memset((*ch)->heads,0,sizeof(SECTION_HEADER)*n+1);
    (*ch)->pos = malloc(sizeof(unsigned int)*n+1);
    return (*ch == NULL) ? 0 : 1;
}

static int fplua_read(lua_State *L){
    CORE_HANDLE **ch = tochp(L);
    if((*ch) != NULL){
        if((*ch)->_io == 0){
            unsigned int n = luaL_checkinteger(L, 2);
            if(n > 0){
                if(n < (*ch)->chead.nofsec+1){
                    unsigned char *data = malloc((*ch)->heads[n-1].size);
                    fseek((*ch)->f,(*ch)->offsets[n-1],SEEK_SET);
                    fread(data,(*ch)->heads[n-1].size,1,(*ch)->f);
                    lua_pushlstring(L,data,(*ch)->heads[n-1].size);
                    free(data);
                    return 1;
                }
            }
        }
    }else{
        closed(L);
    }
    return 0;
}

static int fplua_getnofsec(lua_State *L){
    CORE_HANDLE **ch = tochp(L);
    if(*ch != NULL){
        lua_pushinteger(L,(*ch)->chead.nofsec);
        return 1;
    }else{
        closed(L);
    }
    return 0;
}

static int fplua_getconf(lua_State *L){
    CORE_HANDLE **ch = tochp(L);
    if(*ch != NULL){
        unsigned int data;
        memcpy(&data,(*ch)->chead.conf,sizeof(int));
        lua_pushinteger(L,(IS_BIG_ENDIAN) ? data : __bswap_constant_32(data));
        return 1;
    }else{
        closed(L);
    }
    return 0;
}

static int fplua_setconf(lua_State *L){
    CORE_HANDLE **ch = tochp(L);
    if(*ch != NULL){
        unsigned int data = luaL_checkinteger(L,2);
        if(!IS_BIG_ENDIAN){
            data = __bswap_constant_32(data);
        }
        memcpy(&((*ch)->chead.conf),&data,sizeof(int));
            if((*ch)->_io != 1){
                unsigned int p = (*ch)->Coffset + sizeof(sizeof(int));
                fseek((*ch)->f,p,SEEK_SET);
                fwrite(&(*ch)->chead.conf,sizeof(int),1,(*ch)->f);
            }
    }else{
        closed(L);
    }
    return 0;
}

static int fplua_setsconf(lua_State *L){
    CORE_HANDLE **ch = tochp(L);
    if(*ch != NULL){
        unsigned int n = luaL_checkinteger(L,2);
        if(n > 0 && n < (*ch)->chead.nofsec+1){
            uint8_t c = (uint8_t)luaL_checkinteger(L,3);
            memset(&(*ch)->heads[n-1].Characteristics,c,1);
            if((*ch)->_io != 1){
                unsigned int p = (*ch)->Coffset + sizeof(CORE_HEADER) + ((n-1)*sizeof(SECTION_HEADER)) + sizeof((*ch)->heads[n-1].name);
                fseek((*ch)->f,p,SEEK_SET);
                fwrite(&(*ch)->heads[n-1].Characteristics,sizeof((*ch)->heads[n-1].Characteristics),1,(*ch)->f);
            }
        }
    }else{
        closed(L);
    }
    return 0;
}

static int fplua_getn(lua_State *L){
    CORE_HANDLE **ch = tochp(L);
    if(*ch != NULL){
        if((*ch)->_io == 0){
            const char *name = luaL_checkstring(L, 2);
            for (unsigned int i = 0;i<(*ch)->chead.nofsec;i++){
                if(strcmp((*ch)->heads[i].name,name) == 0){
                    lua_pushinteger(L,i+1);
                    return 1;
                }
            }
        }
    }else{
        closed(L);
    }
    return 0;
}

static int fplua_list(lua_State *L){
    CORE_HANDLE **ch = tochp(L);
    if(*ch != NULL){
            lua_newtable(L);
            for (unsigned int i = 0;i<(*ch)->chead.nofsec;i++){
                lua_newtable(L);
                lua_pushstring(L,(*ch)->heads[i].name);
                lua_rawseti(L,-2,1);
                lua_pushinteger(L,(*ch)->heads[i].size);
                lua_rawseti(L,-2,2);
                lua_pushinteger(L,(*ch)->heads[i].Characteristics);
                lua_rawseti(L,-2,3);
                lua_rawseti(L,-2,i+1);
            }
            return 1;
    }else{
        closed(L);
    }
    return 0;
}

static int fplua_setR(lua_State *L){
    CORE_HANDLE **ch = tochp(L);
    if(*ch != NULL){
        if((*ch)->_io == 0){
            C_ch = *ch;
        }
    }else{
        closed(L);
    }
    return 0;
}

static int plua_fread(lua_State *L){
    unsigned int n = luaL_checknumber(L, 2);
    unsigned int t = luaL_checknumber(L, 3);
    unsigned int c = luaL_checknumber(L, 4);
    if(n > 0 && n < g_ch->chead.nofsec+1){
        fseek(g_ch->f,g_ch->offsets[n-1]+g_ch->pos[n-1],SEEK_SET);
        lua_newtable(L);
        unsigned int p = 0;
        for (unsigned int i = 0;i<c;i++){
            switch (t)
            {
                case 1:
                    {
                        unsigned int s = luaL_checknumber(L, 5);
                        char *data = malloc(s);
                        fread(data,s,1,g_ch->f);
                        lua_pushlstring(L,data,s);
                        lua_rawseti(L,-2,i+1);
                        free(data);
                    }
                    break;
                case 2:{
                        double data;
                        fread(&data,sizeof(double),1,g_ch->f);
                        lua_pushnumber(L,data);
                        lua_rawseti(L,-2,i+1);
                    }
                    break;
                case 3:{
                        int data;
                        fread(&data,sizeof(int),1,g_ch->f);
                        lua_pushinteger(L,data);
                        lua_rawseti(L,-2,i+1);
                    }
                    break;
            }
        }
        return 1;
    }
    return 0;
}

static int fplua_fread(lua_State *L){
    CORE_HANDLE **ch = tochp(L);
    unsigned int n = luaL_checknumber(L, 2);
    unsigned int t = luaL_checknumber(L, 3);
    unsigned int c = luaL_checknumber(L, 4);
    if(*ch != NULL){
            if(n > 0 && n < (*ch)->chead.nofsec+1){
                if((*ch)->_io == 0){
                    fseek((*ch)->f,(*ch)->offsets[n-1]+(*ch)->pos[n-1],SEEK_SET);
                }
                lua_newtable(L);
                unsigned int p = 0;
                for (unsigned int i = 0;i<c;i++){
                    switch (t)
                    {
                        case 1:
                            {
                                unsigned int s = luaL_checknumber(L, 5);
                                char *data = malloc(s);
                                if((*ch)->_io == 0){
                                    fread(data,s,1,(*ch)->f);
                                }else{
                                    if((*ch)->wtype[n-1] == 1){
                                    memcpy(data,(*ch)->data[n-1].data+(*ch)->pos[n-1]+p,s);
                                    p += s;
                                    }else if((*ch)->wtype[n-1] == 2){
                                        FILE *f = fopen((*ch)->data[n-1].filename,"rb");
                                        if(f != NULL){
                                            fseek(f,p,SEEK_SET);
                                            fread(data,s,1,f);
                                            p += s;
                                            fclose(f);
                                        }
                                    }else{
                                        return 0;
                                    }
                                }
                                lua_pushlstring(L,data,s);
                                lua_rawseti(L,-2,i+1);
                                free(data);
                            }
                            break;
                        case 2:{
                                double data;
                                if((*ch)->_io == 0){
                                    fread(&data,sizeof(double),1,(*ch)->f);
                                }else{
                                    if((*ch)->wtype[n-1] == 1){
                                    memcpy(&data,(*ch)->data[n-1].data+(*ch)->pos[n-1]+p,sizeof(double));
                                    p += sizeof(double);
                                    }else if((*ch)->wtype[n-1] == 2){
                                        FILE *f = fopen((*ch)->data[n-1].filename,"rb");
                                        if(f != NULL){
                                            fseek(f,p,SEEK_SET);
                                            fread(&data,sizeof(double),1,f);
                                            p += sizeof(double);
                                            fclose(f);
                                        }
                                    }else{
                                    return 0;
                                    }
                                }
                                lua_pushnumber(L,data);
                                lua_rawseti(L,-2,i+1);
                            }
                            break;
                        case 3:{
                                int data;
                                if((*ch)->_io == 0){
                                    fread(&data,sizeof(int),1,(*ch)->f);
                                }else{
                                    if((*ch)->wtype[n-1] == 1){
                                    memcpy(&data,(*ch)->data[n-1].data+(*ch)->pos[n-1]+p,sizeof(int));
                                    p += sizeof(int);
                                    }else if((*ch)->wtype[n-1] == 2){
                                        FILE *f = fopen((*ch)->data[n-1].filename,"rb");
                                        if(f != NULL){
                                            fseek(f,p,SEEK_SET);
                                            fread(&data,sizeof(int),1,f);
                                            p += sizeof(int);
                                            fclose(f);
                                        }
                                    }else{
                                    return 0;
                                    }
                                }
                                lua_pushinteger(L,data);
                                lua_rawseti(L,-2,i+1);
                            }
                            break;
                    }
                }
                return 1;
        }
    }else{
        closed(L);
    }
    return 0;
}

static int plua_llib(lua_State *L){
    CORE_HANDLE *Old_ch;
    Old_ch = C_ch;
    C_ch = g_ch;
    if(C_ch != NULL){
    const char *name = luaL_checkstring(L, 1);
    const char *init = luaL_checkstring(L, 2);
    int status = llib(L,name,init,0);
    C_ch = Old_ch;
    return status;
    }
	return 0;
}

static int fplua_llib(lua_State *L){
    CORE_HANDLE *Old_ch;
    Old_ch = C_ch;
    CORE_HANDLE **ch = tochp(L);
    C_ch = *ch;
    if(C_ch != NULL){
    const char *name = luaL_checkstring(L, 2);
    const char *init = luaL_checkstring(L, 3);
    int status = llib(L,name,init,0);
    C_ch = Old_ch;
    return status;
    }
	return 0;
}

static int fplua_fseek(lua_State *L){
    CORE_HANDLE **ch = tochp(L);
        unsigned int n = luaL_checknumber(L, 2);
        unsigned int p = luaL_checknumber(L, 3);
        bool t = lua_toboolean(L, 4);
        if(t){
            (*ch)->pos[n-1] = p;
        }else{
            (*ch)->pos[n-1] += p;
        }
    return 0;
}

static int fplua_fwrite(lua_State *L){
    CORE_HANDLE **ch = tochp(L);
    unsigned int n = luaL_checknumber(L, 2);
    unsigned int t = luaL_checknumber(L, 3);
    if(*ch != NULL){
            if(n > 0 && n < (*ch)->chead.nofsec+1){
                    switch (t)
                    {
                        case 1:
                            {
                                size_t si;
                                const char *data = luaL_checklstring (L,4,&si);
                                if((*ch)->_io){
                                    if((*ch)->wtype[n-1] == 1){
                                        memcpy((*ch)->data[n-1].data+(*ch)->pos[n-1],data,si);
                                    }else if((*ch)->wtype[n-1] == 2){
                                        FILE *f = fopen((*ch)->data[n-1].filename,"r+b");
                                        if(f != NULL){
                                            fseek(f,(*ch)->pos[n-1],SEEK_SET);
                                            fwrite(data,si,1,f);
                                            fclose(f);
                                        }
                                    }
                                }else{
                                    fseek((*ch)->f,(*ch)->offsets[n-1]+(*ch)->pos[n-1],SEEK_SET);
                                    fwrite(data,si,1,(*ch)->f);
                                }
                            }
                            break;
                        case 2:{
                                double data = luaL_checknumber(L,4);
                                if((*ch)->_io){
                                    if((*ch)->wtype[n-1] == 1){
                                        memcpy((*ch)->data[n-1].data+(*ch)->pos[n-1],&data,sizeof(double));
                                    }else if((*ch)->wtype[n-1] == 2){
                                        FILE *f = fopen((*ch)->data[n-1].filename,"r+b");
                                        if(f != NULL){
                                            fseek(f,(*ch)->pos[n-1],SEEK_SET);
                                            fwrite(&data,sizeof(double),1,f);
                                            fclose(f);
                                        }
                                    }
                                }else{
                                    fseek((*ch)->f,(*ch)->offsets[n-1]+(*ch)->pos[n-1],SEEK_SET);
                                    fwrite(&data,sizeof(double),1,(*ch)->f);
                                }
                            }
                            break;
                        case 3:{
                                int data = luaL_checknumber(L,4);
                                if((*ch)->_io){
                                    if((*ch)->wtype[n-1] == 1){
                                        memcpy((*ch)->data[n-1].data+(*ch)->pos[n-1],&data,sizeof(int));
                                    }else if((*ch)->wtype[n-1] == 2){
                                        FILE *f = fopen((*ch)->data[n-1].filename,"r+b");
                                        if(f != NULL){
                                            fseek(f,(*ch)->pos[n-1],SEEK_SET);
                                            fwrite(&data,sizeof(int),1,f);
                                            fclose(f);
                                        }
                                    }
                                }else{
                                    fseek((*ch)->f,(*ch)->offsets[n-1]+(*ch)->pos[n-1],SEEK_SET);
                                    fwrite(&data,sizeof(int),1,(*ch)->f);
                                }
                            }
                            break;
                    }
                }
    }else{
        closed(L);
    }
    return 0;
}

static int fplua_rens(lua_State *L){
    CORE_HANDLE **ch = tochp(L);
    if(*ch != NULL){
        unsigned int n = luaL_checknumber(L, 2);
        if(n > 0 && n < (*ch)->chead.nofsec+1){
            const char *name = luaL_checkstring (L,3);
            strcpy((*ch)->heads[n-1].name,name);
            if((*ch)->_io != 1){
                unsigned int p = (*ch)->Coffset + sizeof(CORE_HEADER) + ((n-1)*sizeof(SECTION_HEADER));
                fseek((*ch)->f,p,SEEK_SET);
                fwrite((*ch)->heads[n-1].name,sizeof((*ch)->heads[n-1].name),1,(*ch)->f);
            }
        }
    }
    return 0;
}

static int fplua_alloc(lua_State *L){
    CORE_HANDLE **ch = tochp(L);
    if((*ch)->_io == 1){
        unsigned int n = luaL_checknumber(L, 2);
        unsigned int size = luaL_checknumber(L, 3);
        (*ch)->data[n-1].data = malloc(size);
        memset((*ch)->data[n-1].data,0,size);
        (*ch)->wtype[n-1] = 1;
        (*ch)->heads[n-1].size = size;
        (*ch)->pos[n-1] = 0;
    }
    return 0;
}

static int fplua_setfile(lua_State *L){
    CORE_HANDLE **ch = tochp(L);
    if((*ch)->_io == 1){
        unsigned int n = luaL_checkinteger(L, 2);
        const char *fname = luaL_checkstring(L, 3);
        FILE *f = fopen(fname,"rb");
        if(f != NULL){
        unsigned int fnsize = strlen(fname);
        (*ch)->data[n-1].filename = (char*)malloc(fnsize);
        strcpy((*ch)->data[n-1].filename,fname);
        (*ch)->wtype[n-1] = 2;
        (*ch)->pos[n-1] = 0;
        (*ch)->heads[n-1].size = _filelength(_fileno(f));
        fclose(f);
        }
    }
    return 0;
}

static int fplua_save(lua_State *L){
    CORE_HANDLE **ch = tochp(L);
    if((*ch)->_io == 1){
       const char *fname = luaL_checkstring(L, 2);
       FILE *f = fopen(fname,"wb");
       if(f != NULL){
            fwrite(&luasig,sizeof(SIGNATURE),1,f);
            fwrite(&(*ch)->chead,sizeof((*ch)->chead),1,f);
            fwrite((*ch)->heads,sizeof(SECTION_HEADER),(*ch)->chead.nofsec,f);
            for(unsigned int i = 0;i<(*ch)->chead.nofsec;i++){
                if((*ch)->wtype[i] == 1){
                    fwrite((*ch)->data[i].data,(*ch)->heads[i].size,1,f);
                }else if((*ch)->wtype[i] == 2){
                    FILE *f2 = fopen((*ch)->data[i].filename,"rb");
                    if(f2 != NULL){
                        for(int n = 0;n<(*ch)->heads[i].size;n++){
                            char buff = fgetc(f2);
                            fputc(buff,f);
                        }
                        fclose(f2);
                    }

                }
            }
            fclose(f);
       }
    }
    return 0;
}

static int fplua_close(lua_State *L){
    CORE_HANDLE **ch = tochp(L);
    if((*ch)->_io == 0){
    fclose((*ch)->f);
    }
    free(*ch);
    *ch = NULL;
    ch = NULL;
    return 0;
}

static int fplua_isf(lua_State *L){
    CORE_HANDLE **ch = tochp(L);
    if((*ch)->_io){
        unsigned int n = luaL_checkinteger(L, 2);
        if(n > 0 && n < (*ch)->chead.nofsec+1){
            lua_pushboolean(L,((*ch)->wtype[n-1] == 2));
            return 1;
        }
    }
    return 0;
}

static int plua_resR(lua_State *L){
    C_ch = g_ch;
    return 0;
}

static const luaL_Reg plualib[] = {
  {"loadlib",       plua_llib},
  {"read",          plua_read},
  {"getn",          plua_getn},
  {"list",          plua_list},
  {"load",          plua_load},
  {"new" ,          plua_new},
  {"fread",         plua_fread},
  {"resetrequire",  plua_resR},
  {NULL, NULL}
};
static const luaL_Reg fplualib[] = {
  {"loadlib",       fplua_llib},
  {"read",          fplua_read},
  {"fread",         fplua_fread},
  {"fseek",         fplua_fseek},
  {"fwrite",        fplua_fwrite},
  {"getn",          fplua_getn},
  {"getnofsec",     fplua_getnofsec},
  {"getconf",       fplua_getconf},
  {"setconf",       fplua_setconf},
  {"SetCharacteristics",    fplua_setsconf},
  {"isfile",        fplua_isf},
  {"list",          fplua_list},
  {"setrequire",    fplua_setR},
  {"rename",     fplua_rens},
  {"alloc",      fplua_alloc},
  {"setfile",       fplua_setfile},
  {"savefile",      fplua_save},
  {"close",         fplua_close},
  {NULL, NULL}
};

static void createmeta (lua_State *L) {
  luaL_newmetatable(L, LUA_FPLUAHANDLE);  /* create metatable for pluafile handles */
  lua_pushvalue(L, -1);  /* push metatable */
  lua_setfield(L, -2, "__index");  /* metatable.__index = metatable */
  luaL_register(L, NULL, fplualib);  /* pluafile methods */
}

int luaopen_plua(lua_State *L){
    createmeta(L);
    luaL_register(L, "plua", plualib);
    return 1;
}

static int pmain (lua_State *L) {
  struct Smain *s = (struct Smain *)lua_touserdata(L, 1);
  int argc = s->argc;
  char **argv = s->argv;
  int script;
  int has_i = 0, has_v = 0, has_e = 0;
  globalL = L;
  if (argv[0] && argv[0][0]) progname = argv[0];
  lua_gc(L, LUA_GCSTOP, 0);  /* stop collector during initialization */
  luaL_openlibs(L);  /* open libraries */
  lua_gc(L, LUA_GCRESTART, 0);
  s->status = handle_luainit(L);
  FILE *f = fopen(getprog(),"rb");
  pesize = sizeofpe(f);
  fseek(f,0,SEEK_END);
  filesize = ftell(f);
  if (s->status != 0) return 0;
    if(filesize > pesize){
    SIGNATURE sig;
    fseek(f,pesize,SEEK_SET);
    fread(sig,sizeof(SIGNATURE),1,f);
    if(memcmp(&luasig,&sig,sizeof(SIGNATURE)) == 0){
         g_ch = (CORE_HANDLE*)malloc(sizeof(CORE_HANDLE));
         g_ch->f = f;
         g_ch->Coffset = pesize+sizeof(SIGNATURE);
         C_ch = g_ch;
         int currpos;
         fread(&g_ch->chead,sizeof(CORE_HEADER),1,f);
         if((g_ch->chead.conf[3] & 0x04) == 0x04){luaopen_bit(L);}
         if((g_ch->chead.conf[3] & 0x02) == 0x02){luaopen_plua(L);}
         if(!(g_ch->chead.conf[3] & 0x01)){
            goto luatty;
         }
         g_ch->heads = malloc(sizeof(SECTION_HEADER)*g_ch->chead.nofsec);
         g_ch->offsets = malloc(sizeof(unsigned int)*(g_ch->chead.nofsec+1));
         fread(g_ch->heads,sizeof(SECTION_HEADER),g_ch->chead.nofsec,f);
         currpos = g_ch->Coffset+sizeof(CORE_HEADER)+sizeof(SECTION_HEADER)*g_ch->chead.nofsec;
         for(unsigned int i = 0; i < g_ch->chead.nofsec; i++){
             g_ch->heads[i].name[58] = '\0';
             if(!checkString(g_ch->heads[i].name)){return 0;}
             if(((g_ch->heads[i].Characteristics & 0x10) == 0x10)&& (i != 0)){
                g_ch->offsets[i] = g_ch->offsets[i-1];
                g_ch->heads[i].size = g_ch->heads[i-1].size;
                g_ch->heads[i].Characteristics = g_ch->heads[i-1].Characteristics;
             }else{
             g_ch->offsets[i] = currpos;
             currpos += g_ch->heads[i].size;
             }
         }
         g_ch->offsets[g_ch->chead.nofsec] = currpos;
         for (unsigned int i = 0;i<g_ch->chead.nofsec;i++){
            if(g_ch->heads[i].Characteristics & 0x01){
                load(L,f,g_ch->heads[i].name,g_ch->offsets[i],g_ch->heads[i].size);
            }
         }
    }else{
         load(L,f,"=",pesize,filesize-pesize);
    }
    int i;
    lua_createtable(L,argc,0);
         for (i=0; i<argc; i++)
         {
          lua_pushstring(L,argv[i]);
          lua_rawseti(L,-2,i);
         }
         lua_setglobal(L,"arg");
         luaL_checkstack(L,argc-1,"too many arguments to script");
         for (i=1; i<argc; i++)
         {
          lua_pushstring(L,argv[i]);
         }
         lua_call(L,argc-1,0);
  }else{
  luaopen_plua(L);
  luaopen_bit(L);
  luatty:
  script = collectargs(argv, &has_i, &has_v, &has_e);
  if (script < 0) {  /* invalid args? */
    print_usage();
    s->status = 1;
    return 0;
  }
  if (has_v) print_version();
  s->status = runargs(L, argv, (script > 0) ? script : s->argc);
  if (s->status != 0) return 0;
  if (script)
    s->status = handle_script(L, argv, script);
  if (s->status != 0) return 0;
  if (has_i)
    dotty(L);
  else if (script == 0 && !has_e && !has_v) {
    if (lua_stdin_is_tty()) {
      print_version();
      dotty(L);
    }
    else dofile(L, NULL);  /* executes stdin as a file */
  }
  }
  return 0;
}

int main (int argc, char **argv) {
  int status;
  struct Smain s;
  lua_State *L = lua_open();  /* create state */
  if (L == NULL) {
    l_message(argv[0], "cannot create state: not enough memory");
    return EXIT_FAILURE;
  }
  s.argc = argc;
  s.argv = argv;
  status = lua_cpcall(L, &pmain, &s);
  report(L, status);
  lua_close(L);
  return (status || s.status) ? EXIT_FAILURE : EXIT_SUCCESS;
}

