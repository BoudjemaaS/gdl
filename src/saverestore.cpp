/***************************************************************************
                          saverestore.cpp  -  GDL library procedure
                             -------------------
    begin                : Dec 10 2017
    copyright            : (C) 2017 by Gilles Duvert

 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "includefirst.hpp"
#include "datatypes.hpp"
#include "envt.hpp"
#include "dinterpreter.hpp"
#include "nullgdl.hpp"
#include <queue>

//Useful for debugging...
#define DEBUG_SAVERESTORE 0

using namespace std;

const string rectypes[] = {"START_MARKER", //0
  "COMMON_VARIABLE", //1
  "VARIABLE", //2 
  "SYSTEM_VARIABLE", //3
  "", "", "END_MARKER", //6
  "", "", "", "TIMESTAMP", //10
  "", "COMPILED", //12
  "IDENTIFICATION", //13
  "VERSION", //14
  "HEAP_HEADER", //15
  "HEAP_DATA", //16
  "PROMOTE64", //17
  "", "NOTICE", //19
  "DESCRIPTION", //20
  "UNKNOWN", "UNKNOWN", "UNKNOWN"};

const int sizeOfType[] = {0, 1, 2, 4, 4, 8, 8, 0, 0, 16,0,0,2,4,8,8};

namespace lib {

  using namespace std;
enum {
  START_MARKER=0, // Start of SAVE file
  COMMONBLOCK=1, // Block contains a common block definition
  VARIABLE=2, // Block contains variable data
  SYSTEM_VARIABLE=3, // Block contains system variable data
  END_MARKER=6, // End of SAVE file
  VARSTART=7, //start of variable data
  ARRAYSTART=8,
  STRUCTSTART=9,
  TIMESTAMP=10, //Block contains time stamp information
  COMPILED=12, // Block contains compiled procedure or function
  IDENTIFICATION=13, // Block contains author information
  VERSION_MARKER=14, // Block contains IDL version information
  HEAP_HEADER=15, // Block contains heap index information
  HEAP_DATA=16, // Block contains heap data
  PROMOTE64=17, // Flags start of 64-bit record file offsets
  ARRAYSTART64=18,
  NOTICE=19, // Disclaimer notice
  DESCRIPTION_MARKER=20 //description ?
} Markers;

  typedef std::map<DPtr, SizeT> heapT;
  static heapT heapIndexMapSave; //std::map<DPtr, long> heapIndexMapSave; //list of [ heap pointer, heap index ] used when saving.
  static heapT heapObjIndexMapSave; 
  static std::map<long, std::pair<BaseGDL*,DPtr>> heapIndexMapRestore; //list of [heap index , [variable,heap pointer]] used when reading.
  static SizeT heapIndexSave;
  static SizeT heapObjIndexSave;
  static std::vector<std::string> predeflist;
  static char* saveFileAuthor;
  static char* saveFileDatestring;
  static char* saveFileUser;
  static char* saveFileHost;
  
  static char* notice;
  static int32_t format;
  static  char* arch = 0;
  static  char* os = 0;
  static  char* release = 0;

  static bool safetyTested=false;
  static bool isSafe=false;
  
  static bool save_compress=false;
  static FILE* save_fid=NULL;

#include <rpc/xdr.h>

  // AC 2017-12-13 : missing in  <rpc/xdr.h> for OSX
  // Following https://www.gnu.org/software/gnulib/manual/html_node/xdr_005fint16_005ft.html
  // it may be needed for others OS : Cygwin, Mingw (seems to be OK for *BSD)

#ifdef __APPLE__
#define xdr_uint16_t xdr_u_int16_t
#define xdr_uint32_t xdr_u_int32_t
#define xdr_uint64_t xdr_u_int64_t
#endif

  //this is the routined used by IDL as per the documentation.

  bool_t xdr_complex(XDR *xdrs, DComplex *p) {
    return (xdr_float(xdrs, reinterpret_cast<float *> (p)) && xdr_float(xdrs, reinterpret_cast<float *> (p) + 1));
  }
  //this is the routined used by IDL as per the documentation.

  bool_t xdr_dcomplex(XDR *xdrs, DComplexDbl *p) {
    return (xdr_double(xdrs, reinterpret_cast<double *> (p)) && xdr_double(xdrs, reinterpret_cast<double *> (p) + 1));
  }
  
  void getTimeUserHost(XDR *xdrs) {
    int32_t UnknownLong;
    for (int i = 0; i < 256; ++i) if (!xdr_int32_t(xdrs, &UnknownLong)) break;
    {
      free(saveFileDatestring);
      saveFileDatestring = 0;
      if (!xdr_string(xdrs, &saveFileDatestring, 2048)) cerr << "read error" << endl;
      //      else        fprintf(stderr, "date: \"%s\"\n", saveFileDatestring);
    }
    {
      free(saveFileUser);
      saveFileUser = 0;
      if (!xdr_string(xdrs, &saveFileUser, 2048)) cerr << "read error" << endl;
      //      else        fprintf(stderr, "user: \"%s\"\n", saveFileUser);
    }
    {
      free(saveFileHost);
      saveFileHost = 0;
      if (!xdr_string(xdrs, &saveFileHost, 2048)) cerr << "read error" << endl;
      //      else        fprintf(stderr, "host: \"%s\"\n", saveFileHost);
    }
  }
  
  inline uint32_t writeNewRecordHeader(XDR *xdrs, int code){
    int32_t rectype=code;    
    xdr_int32_t(xdrs, &rectype); //-16
    uint32_t ptrs0=0;
    uint32_t ptrs1=0;
    xdr_uint32_t(xdrs, &ptrs0); //-12 //void, to be updated
    xdr_uint32_t(xdrs, &ptrs1); //-8
    int32_t UnknownLong=0;
    xdr_int32_t(xdrs, &UnknownLong);
    return xdr_getpos(xdrs); //end of header
  }

  inline uint32_t updateNewRecordHeader(XDR *xdrs, uint32_t cur) {
    uint32_t next = xdr_getpos(xdrs);
    //dirty trick for compression: write uncompressed, rewind, read what was just written, compress, write over, reset positions.
    if (save_compress)
    {
      uint32_t uLength = next - cur;
      uLong cLength = compressBound(uLength);
      char* uncompressed = (char*) calloc(uLength+1,1);
      xdr_setpos(xdrs, cur);
      size_t retval = fread(uncompressed, 1, uLength, save_fid);
      if (retval!=uLength) cerr<<"(compress) read error:"<<retval<<"eof:"<<feof(save_fid)<<", error:"<<ferror(save_fid)<<endl;
      char* compressed = (char*) calloc(cLength + 1,1);
      // Deflate
      compress2((Bytef *) compressed, &cLength, (Bytef *) uncompressed, uLength, Z_BEST_SPEED);
      //cLength is the good length now.
      xdr_setpos(xdrs, cur);
      xdr_opaque(xdrs,compressed,cLength);
      next = cur+cLength;
      xdr_setpos(xdrs, next);
      //if (next!=(cur+cLength)) cerr<<"problem:"<<cur+cLength<<":"<<next<<"\n";
    }
    xdr_setpos(xdrs, cur-12); //ptrs0
    xdr_uint32_t(xdrs, &next);
    xdr_setpos(xdrs, next);
    return next;
  }
  
  uint32_t writeTimeUserHost(XDR *xdrs, char* FileDatestring, char* FileUser, char* FileHost) {
    uint32_t cur=writeNewRecordHeader(xdrs, TIMESTAMP);
    int32_t UnknownLong=0;
    for (int i = 0; i < 256; ++i) if (!xdr_int32_t(xdrs, &UnknownLong)) cerr << "write error" << endl;
    if (!xdr_string(xdrs, &FileDatestring, strlen(FileDatestring))) cerr << "write error" << endl;
    if (!xdr_string(xdrs, &FileUser, strlen(FileUser))) cerr << "write error" << endl;
    if (!xdr_string(xdrs, &FileHost, strlen(FileHost))) cerr << "write error" << endl;
    uint32_t next=updateNewRecordHeader(xdrs, cur);
    return next;
  }
  
  uint32_t writeEnd(XDR *xdrs) {
    uint32_t cur=writeNewRecordHeader(xdrs, END_MARKER);
    return cur;
  }

  int getVersion(XDR* xdrs) {
    if (!xdr_int32_t(xdrs, &format)) return 0;
    //    cerr << "Format: " << format << endl;
    free(arch); arch = 0;
    if (!xdr_string(xdrs, &arch, 2048)) return 0;
    //    cerr << arch << endl;
    free(os); os = 0;
    if (!xdr_string(xdrs, &os, 2048)) return 0;
    //    cerr << os << endl;
    free(release); release = 0;
    if (!xdr_string(xdrs, &release, 2048)) return 0;
    //    cerr << release << endl;
    return 1;
  }
  
  uint32_t writeVersion(XDR* xdrs, int32_t *format, char* arch, char* os , char* release) {
    uint32_t cur=writeNewRecordHeader(xdrs, VERSION_MARKER);
    xdr_int32_t(xdrs, format);
    xdr_string(xdrs, &arch, strlen(arch));
    xdr_string(xdrs, &os, strlen(os));
    xdr_string(xdrs, &release, strlen(release));
    uint32_t next=updateNewRecordHeader(xdrs, cur);
    return next;
  }
  
  int getNotice(XDR* xdrs) {
    free(notice); notice = 0;
    if (!xdr_string(xdrs, &notice, 20480)) return 0;
//    cerr << notice << endl;
    return 1;
  }
  
  uint32_t writeNotice(XDR* xdrs, char* notice) {
    uint32_t cur=writeNewRecordHeader(xdrs, NOTICE);
    xdr_string(xdrs, &notice, strlen(notice));
    uint32_t next=updateNewRecordHeader(xdrs, cur);
    return next;
  }

  char* getDescription(XDR *xdrs) {
    int32_t length = 0;
    if (!xdr_int32_t(xdrs, &length)) cerr << "error reading description string length" << endl;
    if (length > 0)
    {
      char* chars = 0;
      if (!xdr_string(xdrs, &chars, length)) cerr << "error getting string" << endl;
      return chars;
    } else return NULL;
  }

  uint32_t writeDescription(XDR *xdrs, char* descr) {
    uint32_t cur=writeNewRecordHeader(xdrs, DESCRIPTION_MARKER);
    int32_t length = strlen(descr);
    if (!xdr_int32_t(xdrs, &length)) cerr << "error writing description string length" << endl;
    if (!xdr_string(xdrs, &descr, length)) cerr << "error writing string" << endl;
    uint32_t next=updateNewRecordHeader(xdrs, cur);
    return next;
   }

  int getIdentification(XDR *xdrs) {
    free(saveFileAuthor);
    saveFileAuthor = 0;
    if (!xdr_string(xdrs, &saveFileAuthor, 2048)) return 0;
    //    cerr << author << endl;
    char* title = 0;
    if (!xdr_string(xdrs, &title, 2048)) return 0;
    //    cerr << title << endl;
    char* otherinfo = 0;
    if (!xdr_string(xdrs, &otherinfo, 2048)) return 0;
    //    cerr << otherinfo << endl;
    return 1;
  }
  
  uint32_t writeIdentification(XDR *xdrs, char *saveFileAuthor, char* title, char* otherinfo ) {
    uint32_t cur=writeNewRecordHeader(xdrs, IDENTIFICATION);
    xdr_string(xdrs, &saveFileAuthor, strlen(saveFileAuthor));
    xdr_string(xdrs, &title, strlen(title) );
    xdr_string(xdrs, &otherinfo, strlen(otherinfo) );
    uint32_t next=updateNewRecordHeader(xdrs, cur);
    return next;
  }
  
  dimension* getArrDesc64(XDR* xdrs) {
    if (DEBUG_SAVERESTORE) cerr << "get 64-Bits Array descriptor ," ;
    int64_t UnknownLong;
    if (!xdr_int64_t(xdrs, &UnknownLong)) return NULL;
    int64_t nbytes;
    if (!xdr_int64_t(xdrs, &nbytes)) return NULL;
    int64_t nEl;
    if (!xdr_int64_t(xdrs, &nEl)) return NULL;
    int32_t nDims;
    if (!xdr_int32_t(xdrs, &nDims)) return NULL; //on 32 bits
    if (!xdr_int64_t(xdrs, &UnknownLong)) return NULL; //ignored as we ignore the 2x32 bits integers in the other version.
    if (DEBUG_SAVERESTORE) cerr << "nbytes:" << nbytes << " ,nEl:" << nEl << ", nDims:" << nDims<<" ";
    int64_t dims[8];
    if (!xdr_vector(xdrs, (char*) dims, 8, sizeof (int64_t), (xdrproc_t) xdr_int64_t)) return NULL;
    SizeT k = dims[0];
    dimension* theDim = new dimension(k);
    for (int i = 1; i < 8; ++i)
    {
      k = dims[i];
      *theDim << k;
    }
    theDim->Purge();
    if (DEBUG_SAVERESTORE) cerr<<*theDim<<endl;
    return theDim;
  }

  dimension* getArrDesc(XDR* xdrs) {
    if (DEBUG_SAVERESTORE) cerr << "get standard 32-Bits Array descriptor ," ;
    int32_t arrstart;
    int32_t UnknownLong;
    if (!xdr_int32_t(xdrs, &arrstart)) return NULL;
    if (arrstart != ARRAYSTART && arrstart != ARRAYSTART64) //'10'o and '22'o
    {
      cerr << "array is not a array! abort." << endl;
      return 0;
    }
    if (arrstart == ARRAYSTART64) return getArrDesc64(xdrs); //as the rest is specially coded on 8 bytes.
    
    if (!xdr_int32_t(xdrs, &UnknownLong)) return NULL;
    int32_t nbytes;
    if (!xdr_int32_t(xdrs, &nbytes)) return NULL;
    int32_t nEl;
    if (!xdr_int32_t(xdrs, &nEl)) return NULL;
    int32_t nDims;
    if (!xdr_int32_t(xdrs, &nDims)) return NULL;
    if (!xdr_int32_t(xdrs, &UnknownLong)) return NULL;
    if (!xdr_int32_t(xdrs, &UnknownLong)) return NULL;
    int32_t nmax;
    if (!xdr_int32_t(xdrs, &nmax)) return NULL;
    if (DEBUG_SAVERESTORE) cerr << "nbytes:" << nbytes << " ,nEl:" << nEl << ", nDims:" << nDims<<" ";
    int32_t dims[nmax];
    if (!xdr_vector(xdrs, (char*) dims, nmax, sizeof (int32_t), (xdrproc_t) xdr_int32_t)) return NULL;
    SizeT k = dims[0];
    dimension* theDim = new dimension(k);
    for (int i = 1; i < nmax; ++i)
    {
      k = dims[i];
      *theDim << k;
    }
    theDim->Purge();
    if (DEBUG_SAVERESTORE)    cerr<<*theDim<<endl;
    return theDim;
  }

  void writeArrDesc64(XDR* xdrs, BaseGDL* var) {
    int32_t arrstart=ARRAYSTART64;
    xdr_int32_t(xdrs, &arrstart);
    //very important:
    int64_t typeLength=sizeOfType[var->Type()];
    if (var->Type()==GDL_STRING) typeLength=(var->NBytes()/var->N_Elements())-1;
    if (typeLength==0) typeLength=(var->NBytes());
    xdr_int64_t(xdrs, &typeLength);
    int64_t nbytes=var->NBytes();
    xdr_int64_t(xdrs, &nbytes);
    int64_t nEl=var->N_Elements();
    xdr_int64_t(xdrs, &nEl);
    int32_t nDims=var->Rank();
    if (nDims==0 && var->Type()==GDL_STRUCT ) nDims=1;
    xdr_int32_t(xdrs, &nDims);
    int32_t UnknownLong=0;
    xdr_int32_t(xdrs, &UnknownLong);
    xdr_int32_t(xdrs, &UnknownLong);
    int64_t nmax=8;
    int64_t dims[nmax];
    for (int i=0; i < nmax; ++i) dims[i]=1; //yes.
    // not written xdr_int32_t(xdrs, &nmax);
    for (int i=0; i < nDims; ++i) if (var->Dim(i) > 0) dims[i]=var->Dim(i);
    if (DEBUG_SAVERESTORE) std::cerr<<var->Dim()<<std::endl;
    xdr_vector(xdrs, (char*) dims, nmax, sizeof (int64_t), (xdrproc_t) xdr_int64_t);
  }

  void writeArrDesc32(XDR* xdrs, BaseGDL* var) {
    int32_t arrstart=ARRAYSTART;
    xdr_int32_t(xdrs, &arrstart);
    //very important:
    int32_t typeLength=sizeOfType[var->Type()];
    if (var->Type()==GDL_STRING) typeLength=(var->NBytes()/var->N_Elements())-1;
    if (typeLength==0) typeLength=(var->NBytes());
    xdr_int32_t(xdrs, &typeLength);
    int32_t nbytes=var->NBytes();
    xdr_int32_t(xdrs, &nbytes);
    int32_t nEl=var->N_Elements();
    xdr_int32_t(xdrs, &nEl);
    int32_t nDims=var->Rank();
    if (nDims==0 && var->Type()==GDL_STRUCT ) nDims=1;
    xdr_int32_t(xdrs, &nDims);
    int32_t UnknownLong=0;
    xdr_int32_t(xdrs, &UnknownLong);
    xdr_int32_t(xdrs, &UnknownLong);
    int32_t nmax=8;
    int32_t dims[nmax];
    for (int i=0; i < nmax; ++i) dims[i]=1; //yes.
    xdr_int32_t(xdrs, &nmax);
    for (int i=0; i < nDims; ++i) if (var->Dim(i) > 0) dims[i]=var->Dim(i);
    if (DEBUG_SAVERESTORE) std::cerr<<var->Dim()<<std::endl;
    xdr_vector(xdrs, (char*) dims, nmax, sizeof (int32_t), (xdrproc_t) xdr_int32_t);
  }
  
  void writeArrDesc(XDR* xdrs, BaseGDL* var) {
    //very important check total size and switch if size is >2GO. 
    //Warning do not use a SizeT as typelength as this does not like negative values returned by for 0 length strings
    DLong64 typeLength=sizeOfType[var->Type()];
    if (var->Type()==GDL_STRING) typeLength=(var->NBytes()/var->N_Elements()); 
    SizeT nEl=var->N_Elements();
    if (nEl*typeLength > 2000000000ULL) writeArrDesc64(xdrs,var); else writeArrDesc32(xdrs,var);
  }
  
  int defineCommonBlock(EnvT* e, XDR* xdrs, int verboselevel) {
    int32_t ncommonvars;
    if (!xdr_int32_t(xdrs, &ncommonvars)) return 0;
    char* commonname = 0;
    if (!xdr_string(xdrs, &commonname, 2048)) return 0;
    char* varnames[ncommonvars];
    for (int i = 0; i < ncommonvars; ++i) varnames[i] = 0;
    for (int i = 0; i < ncommonvars; ++i) if (!xdr_string(xdrs, &varnames[i], 2048)) return 0;

    if (verboselevel>1){
      cerr << "Common " << commonname << " consists of the following variables:" << endl;
      for (int i = 0; i < ncommonvars; ++i) cerr << varnames[i] << ",";
      cerr << endl;
    }
    //populate common block

    //behaviour is: if one of these variables already exist, the whole common is not defined, and
    //the common variables become just normal variables:

    EnvStackT& callStack = e->Interpreter()->CallStack();
    int32_t curlevnum = callStack.size();
    DSubUD* pro = static_cast<DSubUD*> (callStack[curlevnum - 1]->GetPro());

    for (int i = 0; i < ncommonvars; ++i)
    {
      std::string varName = varnames[i];
      if (pro->FindVar(varName) >= 0)
      {
        Message(varName + " is already defined with a conflicting definition.");
        return 1; //this should not produce an error.
      }
    }

    std::string commonName = commonname;

    if (pro->Common(commonName) == NULL)
    { //does not exist: create
      DCommon* newCommon = new DCommon(commonName);
      pro->AddCommon(newCommon);
      if (verboselevel>0) Message("Restored common block: " + commonName);
    }
    commonName.clear();
    DCommonBase* currentcommon = pro->Common(std::string(commonname));
    for (int i = 0; i < ncommonvars; ++i)
    {
      std::string varName = varnames[i];
      currentcommon->AddVar(varName);
      varName.clear();
    }

    return 1;
  }
  
  DStructGDL* getDStruct(EnvT* e, XDR* xdrs, dimension* inputdims, bool &isObjStruct) {
    isObjStruct=false;
    int32_t structstart;
    if (!xdr_int32_t(xdrs, &structstart)) return NULL;
    if (structstart != STRUCTSTART)
    {
      cerr << "structure is not a structure! abort." << endl;
      return NULL;
    }
    char* structname = 0;
    if (!xdr_string(xdrs, &structname, 2048)) return NULL;
    int32_t structure_def_flags;
    if (!xdr_int32_t(xdrs, &structure_def_flags)) return NULL;
    bool ispredef = false;
    if (structure_def_flags & 0x01)
    {
      ispredef = true;
    }
    bool unknown_flag = false;
    bool is_super = false;
    bool inherits = false;
    if (structure_def_flags & 0x08)
    {
      unknown_flag = true;
    }
    if (structure_def_flags & 0x02)
    {
      inherits = true;
    }
    if (structure_def_flags & 0x04)
    {
      is_super = true;
    }
    
    if (is_super || inherits) isObjStruct=true;
    if (DEBUG_SAVERESTORE) {
     if (isObjStruct) {
       cerr<<std::hex<<structure_def_flags<<std::dec<<std::endl;
       cerr << "Object name:\"" << structname << "\"";
       if (inherits) cerr << ", inherits";
       if (is_super) cerr << ", is a Superclass";
     } else {
       cerr << "Structure name:\"" << structname << "\"";
     }
     if (unknown_flag) cerr << ", is tagged by unknown flag 0x08";
     if (ispredef) cerr << ", is already defined";
     cerr<<endl;
    }

    int32_t ntags;
    if (!xdr_int32_t(xdrs, &ntags)) return NULL;
    int32_t struct_nbytes;
    if (!xdr_int32_t(xdrs, &struct_nbytes)) return NULL;
    //    cerr << "ntags=" << ntags << ",nbytes=" << struct_nbytes << endl;
    //if predef == 1 this ends the Struct_desc, meaning that the definition of such a
    //structure has already been presented and we should reuse it.
    // otherwise, we define the structure using the following entries:
    if (ispredef)
    {
      std::string name = std::string(structname);
      if (name != "$truct") // named struct
        name = StrUpCase(name);
      DStructDesc* desc =
        e->Interpreter()->GetStruct(name, e->CallingNode()); //will throw if does not exist.

      return new DStructGDL(desc, *inputdims);
    } else
    {
      //TAG_DESC repated ntags times:
      int32_t tag_typecode[ntags];
      char* tag_name[ntags];
      int32_t tag_flag[ntags];
      int32_t tag_offset[ntags];
      for (int i = 0; i < ntags; ++i) tag_name[i] = 0;
      for (int i = 0; i < ntags; ++i)
      {
        //TAG_DESC:
        if (!xdr_int32_t(xdrs, &tag_offset[i])) break;
        if (!xdr_int32_t(xdrs, &tag_typecode[i])) break;
        if (!xdr_int32_t(xdrs, &tag_flag[i])) break;
      }
      for (int i = 0; i < ntags; ++i)
      {
        //TAGNAMES x NTAGS:
        if (!xdr_string(xdrs, &tag_name[i], 2048)) break;
      }

      int32_t narrays = 0;
      int32_t nstructs = 0;
      for (int i = 0; i < ntags; ++i) if (tag_flag[i] & 0x20) nstructs++;
      for (int i = 0; i < ntags; ++i) if (tag_flag[i] & 0x04) narrays++;
      dimension * tagdimensions[narrays + 1]; //Always >0: FIXME IF MEMORY LEAK!
      //      cerr<<"reading "<<nstructs<<" structs and "<<narrays<<" arrays."<<endl;
      //if there are any tag flags indicating the tag is an array, read the ARRDESC
      for (int i = 0; i < narrays; ++i)
      {
        tagdimensions[i] = getArrDesc(xdrs);
        if (tagdimensions[i] == NULL) return NULL;
      }

      std::string stru_name = std::string(structname);
      DStructDesc* stru_desc;
      DStructDesc* ref_desc;
      bool checkStruct=false;
      //take care of named structures
      if (stru_name.length() > 0 && stru_name[0] != '$')
      {
        stru_desc = FindInStructList(structList, stru_name);

        if (stru_desc == NULL)
        {
          stru_desc = new DStructDesc(stru_name);
          structList.push_back(stru_desc);
        } else {
          checkStruct=true;
          ref_desc=stru_desc;
          stru_desc=new DStructDesc("$truct"); //make it anonymous, test if equality at end!
        }
      } else stru_desc=new DStructDesc("$truct");
      //summary & tag population:
      for (int i = 0, j = 0, k = 0; i < ntags; ++i)
      {
        //reserved a dimension
        dimension pardim = dimension();

        if (tag_flag[i] & 0x04)
        {
          // modify pardim and push index;
          pardim = *(tagdimensions[j++]); //memory leak?
          //          cerr << " " << pardim << endl;
        }

        switch (tag_typecode[i]) {
          case GDL_BYTE: //	Byte
          {
            SpDByte entry(pardim);
            stru_desc->AddTag(tag_name[i], &entry);
          }
            break;

          case GDL_INT: //	16-bit Integer 
          {
            SpDInt entry(pardim);
            stru_desc->AddTag(tag_name[i], &entry);
          }
            break;

          case GDL_LONG: //	32-bit Long Integer 
          {
            SpDLong entry(pardim);
            stru_desc->AddTag(tag_name[i], &entry);
          }
            break;

          case GDL_FLOAT: //	32-bit Floating Point Number 
          {
            SpDFloat entry(pardim);
            stru_desc->AddTag(tag_name[i], &entry);
          }
            break;

          case GDL_DOUBLE: //	64-bit Floating Point Number 
          {
            SpDDouble entry(pardim);
            stru_desc->AddTag(tag_name[i], &entry);
          }
            break;

          case GDL_COMPLEX: //	Complex Floating Point Number (32-bits each) 
          {
            SpDComplex entry(pardim);
            stru_desc->AddTag(tag_name[i], &entry);
          }
            break;
          case GDL_STRING: //	String
          {
            SpDString entry(pardim);
            stru_desc->AddTag(tag_name[i], &entry);
          }
            break;
          case GDL_STRUCT: //	Structure (never a scalar)
          {
            bool dummy;
            DStructGDL* parStruct = getDStruct(e, xdrs, &pardim, dummy);
            if (parStruct == NULL) return NULL;
            stru_desc->AddTag(tag_name[i], parStruct);
          }
            break;

          case GDL_COMPLEXDBL: //	Complex Floating Point Number (64-bits each) 
          {
            SpDComplexDbl entry(pardim);
            stru_desc->AddTag(tag_name[i], &entry);
          }
            break;

          case GDL_PTR: //	Heap Pointer 
          {
            DPtrGDL entry(pardim);
            stru_desc->AddTag(tag_name[i], &entry);
          }
            break;
          case GDL_OBJ: //	Object Reference (not supported by CMSVLIB)
          {
            DObjGDL entry(pardim);
            stru_desc->AddTag(tag_name[i], &entry);
          }
            break;
          case GDL_UINT: //	16-bit Unsigned Integer 
          {
            SpDUInt entry(pardim);
            stru_desc->AddTag(tag_name[i], &entry);
          }
            break;

          case GDL_ULONG: //	32-bit Unsigned Integer 
          {
            SpDULong entry(pardim);
            stru_desc->AddTag(tag_name[i], &entry);
          }
            break;

          case GDL_LONG64: //	64-bit Integer 
          {
            SpDLong64 entry(pardim);
            stru_desc->AddTag(tag_name[i], &entry);
          }
            break;

          case GDL_ULONG64: //	64-bit Unsigned Integer 
          {
            SpDULong64 entry(pardim);
            stru_desc->AddTag(tag_name[i], &entry);
          }
            break;

          default: //	0 ? Undefined (not allowed) 
            //            cerr << "Should not happen" << endl;
            break;
        }



        //        cerr << endl;
      }

      //Then there should be CLASSNAME if INHERITS or IS_SUPER 
      if (isObjStruct || is_super)
      {
        char* classname = 0;
        if (!xdr_string(xdrs, &classname, 2048)) return NULL;
        if (DEBUG_SAVERESTORE) cerr << "CLASSNAME: \"" << classname << "\"" << endl;
        //NSUPCLASSES:
        int32_t nsupclasses = 0;
        if (!xdr_int32_t(xdrs, &nsupclasses)) return NULL;
        if (DEBUG_SAVERESTORE)  cerr << "NSUPCLASSES=" << nsupclasses << endl;
        if (nsupclasses > 0)
        {
          for (int i = 0; i < nsupclasses; ++i)
          {
            char* supclassname = 0;
            if (!xdr_string(xdrs, &supclassname, 2048)) return NULL;
            if (DEBUG_SAVERESTORE) cerr << "SUPCLASSNAME " << i << ": " << supclassname << endl;
          }
          for (int i = 0; i < nsupclasses; ++i)
          {
            //define all parent classes in objheap.
            DStructGDL* superclass = getDStruct(e, xdrs, new dimension(1), isObjStruct); // will define the class as an object.
            if (isObjStruct)  {
              DPtr ptr= e->NewObjHeap(1, static_cast<DStructGDL*>(superclass));
            }
          }
        }
      }
      if (checkStruct)
      {
        try
        {
          ref_desc->AssureIdentical(stru_desc);
          stru_desc=ref_desc; //OK, switch back.
        }        catch (GDLException& ex)
        {
          return NULL;
        }
      }
      return new DStructGDL(stru_desc, *inputdims);
    }
  }
  
  void writeStructDesc(XDR* xdrs, DStructGDL* var, bool isObject=false, bool is_super=false) {
    DStructDesc* str=var->Desc();
    int32_t structstart=STRUCTSTART;
    xdr_int32_t(xdrs, &structstart);
    const char* structname = str->Name().c_str();
    if (DEBUG_SAVERESTORE && isObject) cerr << "Writing Object \""<< str->Name() <<"\" @"<< var << endl;
    if (DEBUG_SAVERESTORE && !isObject) cerr << "Writing Structure \""<< str->Name() <<"\" @"<< var << endl;
    //predef: struct name is known
    bool ispredef = (str->Name() != "$truct"); // named struct
    //void name if anonymous struct!
    if (!ispredef) {
      std::string nullstr=""; char* voidchar=(char*)nullstr.c_str();
      xdr_string(xdrs,(char**)&voidchar, 0);
      }
    else xdr_string(xdrs, (char**) &structname, str->Name().size());
    //now, did we already define this named structure in the file?
    //if no, add name to list of defined (for further use) and remove is_predef (to write it).
    if (ispredef) {
      bool found=false;
      for (SizeT i=0; i<predeflist.size(); ++i)
      {
        if (predeflist[i]==str->Name()) {found=true; break;}
      }
      if (!found) {
        predeflist.push_back(str->Name());
        ispredef=false;
      }
    }
    //flags.

    int32_t structure_def_flags=0;
    if (ispredef) structure_def_flags |= 0x01;
    if (isObject) structure_def_flags |= 0x08; //it is a CLASS
    if (isObject) structure_def_flags |= 0x02;
    if (is_super) structure_def_flags |= 0x04;
    xdr_int32_t(xdrs, &structure_def_flags);
    int32_t ntags=str->NTags();
    xdr_int32_t(xdrs, &ntags);
    int32_t struct_nbytes=(ispredef)?str->NBytes():0;
    xdr_int32_t(xdrs, &struct_nbytes);
    //if predef == 1  this ends the Struct_desc, meaning that we have already presented this structure.
    if (ispredef) return;
    //TAG_DESC repated ntags times:
    int32_t tag_typecode[ntags];
    char* tag_name[ntags];
    int32_t tag_flag[ntags];
    int32_t tag_offset[ntags];
    int32_t byteoff=0;
    for (int i = 0; i < ntags; ++i) tag_name[i] = (char*) str->TagName(i).c_str();
    for (SizeT i = 0; i < ntags; ++i) { tag_offset[i] = byteoff; byteoff+=var->GetTag(i)->NBytes();} //probably OK
    for (int i = 0; i < ntags; ++i)
    {
      tag_flag[i]=0;
      if (var->GetTag(i,0)->Rank()> 0) tag_flag[i]|=0x04;
      switch (var->GetTag(i,0)->Type()) {
        case GDL_STRUCT: 
          tag_flag[i] |=0x20;
        default: 
          tag_typecode[i] = var->GetTag(i,0)->Type();
      }
    }
    for (int i = 0; i < ntags; ++i)
    {
      //TAG_DESC:
      xdr_int32_t(xdrs, &tag_offset[i]);
      xdr_int32_t(xdrs, &tag_typecode[i]);
      xdr_int32_t(xdrs, &tag_flag[i]);
    }
    for (int i = 0; i < ntags; ++i)
    {
      //TAGNAMES x NTAGS:
      xdr_string(xdrs, &tag_name[i], strlen(tag_name[i]));
    }
    //ARRDESC x NARRAYS:
    for (int i = 0; i < ntags; ++i)
    {
      if (tag_flag[i] & 0x04) writeArrDesc(xdrs, var->GetTag(i));
    }
    //STRUCTDESC x NARRAYS:
    for (int i = 0; i < ntags; ++i)
    {
      if (tag_flag[i] & 0x20) writeStructDesc(xdrs, static_cast<DStructGDL*>(var->GetTag(i)));
    }
    //TBD: CLASSES
    if (isObject) {
      xdr_string(xdrs, (char**) &structname, str->Name().size()); //CLASSNAME
      if (DEBUG_SAVERESTORE) std::cerr << "Object CLASS is \""<< str->Name() << std::endl;
      std::vector< std::string> pNames;
      str->GetParentNames(pNames);
//TBD: get super classes and write structs accordingly. 
      int32_t nsupclasses=pNames.size(); //do they exist???
      for (int i=0 ; i< nsupclasses;++i) {
          DStructGDL* parent = new DStructGDL( pNames[i]);
          if (parent == NULL) nsupclasses--;
          GDLDelete(parent);
        }      
      xdr_int32_t(xdrs, &nsupclasses);
      if (nsupclasses > 0) {
        DStructGDL* parents[nsupclasses];
        // AC not OK on OSX12: std::string pnames[nsupclasses];
	string *pnames = new string[nsupclasses];
        int k=0;
        for (int i=0 ; i< pNames.size();++i) {
          DStructGDL* parent = new DStructGDL( pNames[i]);
          if (parent != NULL) {
            pnames[k]=pNames[i];
            parents[k]=parent;
            if (DEBUG_SAVERESTORE) std::cerr << "     SUPCLASS:  \""<< pNames[k] << std::endl;
            k++;
          }
        }
        for (int i=0 ; i< nsupclasses;++i) {char* str=(char*)(pnames[i].c_str()); xdr_string(xdrs, &str,pnames[i].size());}
        for (int i=0 ; i< nsupclasses;++i) {
            DStructGDL* parent = parents[i];
            Guard<DStructGDL> parent_guard(parent);
            writeStructDesc(xdrs, parent, true, true);
          }
	delete [] pnames;       
	}
    }
  }
  
  
  BaseGDL* getVariable(EnvT* e, XDR* xdrs, int &isSysVar, bool &isObjStruct) {
    bool isStructure = false;
    bool isArray = false;
    // start of TYPEDESC
    // common for VARIABLE, SYSTEM_VARIABLE and HEAP_DATA:
    // 1) TYPECODE
    int32_t typecode;
    if (!xdr_int32_t(xdrs, &typecode)) return NULL;
    // 2) VARFLAGS
    int32_t varflags;
    if (!xdr_int32_t(xdrs, &varflags)) return NULL;

    if (varflags & 0x40) return NullGDL::GetSingleInstance(); //special !NULL variable, no variable content follows.

    if (varflags & 0x02) //defines a system variable.
    {
      isSysVar |= 0x02;
//           cerr << " system " << endl;
    }
    if (varflags & 0x01)
    {
      isSysVar |= 0x01;
//            cerr << " readonly " << endl;
    }


    if (varflags & 0x20)
    {
      isStructure = true; //may also be an array!
    }
    else if (varflags & 0x04)
    {
      isArray = true;
    }
    //This is not signaled in C. Marqwardt doc: a system variable has two supplemental int32 (0x04 and 0x02) here, that we skip.
    if (isSysVar & 0x02)
    {
      int32_t dummy;
      if (!xdr_int32_t(xdrs, &dummy)) return NULL;
      if (!xdr_int32_t(xdrs, &dummy)) return NULL;
    }
    //we gonnna create a BaseGDL:

    BaseGDL* var;
    dimension* dims;
    // if ARRAY or STRUCTURE, Read ARRAY_DESC that follows:
    if (isStructure)
    {//if This was a Structure, it has an ARRAY_DESC plus a STRUCT_DESC 
      dims = getArrDesc(xdrs);
      if (dims == NULL) return NULL;
      var = getDStruct(e, xdrs, dims, isObjStruct);
    } else
    {
      if (isArray)
      { //and NOT a structure...
        dims = getArrDesc(xdrs); 
        if (dims == NULL) return NULL;
      } else
      { //normal plain variable
        dims = new dimension();
      }
      switch (typecode) {
        case GDL_BYTE: //	Byte
          var = new DByteGDL(*dims);
          break;
        case GDL_INT: //	16-bit Integer 
          var = new DIntGDL(*dims);
          break;
        case GDL_LONG: //	32-bit Long Integer 
          var = new DLongGDL(*dims);
          break;
        case GDL_FLOAT: //	32-bit Floating Point Number 
          var = new DFloatGDL(*dims);
          break;
        case GDL_DOUBLE: //	64-bit Floating Point Number 
          var = new DDoubleGDL(*dims);
          break;
        case GDL_COMPLEX: //	Complex Floating Point Number (32-bits each) 
          var = new DComplexGDL(*dims);
          break;
        case GDL_STRING: //	String
          var = new DStringGDL(*dims);
          break;
        case GDL_STRUCT: //	Structure (never a scalar)
          cerr << "Should not happen: struct" << endl;
          break;
        case GDL_COMPLEXDBL: //	Complex Floating Point Number (64-bits each) 
          var = new DComplexDblGDL(*dims);
          break;
        case GDL_PTR: //	Heap Pointer 
          var = new DPtrGDL(*dims);
          break;
        case GDL_OBJ: //	Object Reference (not supported by CMSVLIB) 
          var = new DObjGDL(*dims);
          break;
        case GDL_UINT: //	16-bit Unsigned Integer 
          var = new DUIntGDL(*dims);
          break;
        case GDL_ULONG: //	32-bit Unsigned Integer 
          var = new DULongGDL(*dims);
          break;
        case GDL_LONG64: //	64-bit Integer 
          var = new DLong64GDL(*dims);
          break;
        case GDL_ULONG64: //	64-bit Unsigned Integer 
          var = new DULong64GDL(*dims);
          break;
        default: //	0 ? Undefined (not allowed) 
          //          cerr <<"Should not happen"<<endl;
          break;
      }
    }

    return var;
  }

  void fillVariableData(XDR* xdrs, BaseGDL* var) {
    u_int nEl = var->N_Elements();
    switch (var->Type()) {

      case GDL_BYTE:
      {
        char* addr = (char*) var->DataAddr();
        if (!xdr_bytes(xdrs, &addr, &nEl, nEl)) cerr << "error GDL_BYTE" << endl;
      }
        break;
      case GDL_INT:
      {
        if (!xdr_vector(xdrs, (char*) var->DataAddr(), nEl, sizeof (DInt), (xdrproc_t) xdr_int16_t)) cerr << "error GDL_INT" << endl;
      }
        break;
      case GDL_UINT:
      {
        if (!xdr_vector(xdrs, (char*) var->DataAddr(), nEl, sizeof (DUInt), (xdrproc_t) xdr_uint16_t)) cerr << "error GDL_UINT" << endl;
      }
        break;
      case GDL_LONG:
      {
        if (!xdr_vector(xdrs, (char*) var->DataAddr(), nEl, sizeof (int32_t), (xdrproc_t) xdr_int32_t)) cerr << "error GDL_LONG" << endl;
      }
        break;
      case GDL_ULONG:
      {
        if (!xdr_vector(xdrs, (char*) var->DataAddr(), nEl, sizeof (DULong), (xdrproc_t) xdr_uint32_t)) cerr << "error GDL_ULONG" << endl;
      }
        break;
      case GDL_LONG64:
      {
        if (!xdr_vector(xdrs, (char*) var->DataAddr(), nEl, sizeof (DLong64), (xdrproc_t) xdr_int64_t)) cerr << "error GDL_LONG64" << endl;
      }
        break;
      case GDL_ULONG64:
      {
        if (!xdr_vector(xdrs, (char*) var->DataAddr(), nEl, sizeof (DULong64), (xdrproc_t) xdr_uint64_t)) cerr << "error GDL_ULONG64" << endl;
      }
        break;
      case GDL_FLOAT:
      {
        if (!xdr_vector(xdrs, (char*) var->DataAddr(), nEl, sizeof (DFloat), (xdrproc_t) xdr_float)) cerr << "error GDL_FLOAT" << endl;
      }
        break;
      case GDL_DOUBLE:
      {
        if (!xdr_vector(xdrs, (char*) var->DataAddr(), nEl, sizeof (DDouble), (xdrproc_t) xdr_double)) cerr << "error GDL_DOUBLE" << endl;
      }
        break;
      case GDL_COMPLEX:
      {
        u_int nEl2 = nEl * 2;
        if (!xdr_vector(xdrs, (char*) var->DataAddr(), nEl2, sizeof (DFloat), (xdrproc_t) xdr_float)) cerr << "error GDL_COMPLEX" << endl;
      }
        break;
      case GDL_COMPLEXDBL:
      {
        u_int nEl2 = nEl * 2;
        if (!xdr_vector(xdrs, (char*) var->DataAddr(), nEl2, sizeof (DDouble), (xdrproc_t) xdr_double)) cerr << "error GDL_COMPLEXDBL" << endl;
      }
        break;
      case GDL_STRING:
      {
        for (SizeT i = 0; i < nEl; ++i)
        {
          int32_t length;
          if (!xdr_int32_t(xdrs, &length)) cerr << "error reading string length" << endl;
          if (length > 0)
          {
            char* chars = 0;
            if (!xdr_string(xdrs, &chars, length)) cerr << "error getting string" << endl;
            (*static_cast<DStringGDL*> (var))[i].assign(chars);
          }
        }
      }
        break;
      case GDL_STRUCT:
      {
        DStructGDL* str = static_cast<DStructGDL*> (var);
        SizeT nTags = str->Desc()->NTags();
        for (SizeT ix = 0; ix < nEl; ++ix) for (SizeT t = 0; t < nTags; ++t) fillVariableData(xdrs, str->GetTag(t, ix));
        break;
      }
      case GDL_PTR:
      {
        int32_t heapNumber[nEl];
        DPtrGDL* ptr = static_cast<DPtrGDL*> (var);
        for (SizeT ix = 0; ix < nEl; ++ix) xdr_int32_t(xdrs, &(heapNumber[ix]));
        for (SizeT ix = 0; ix < nEl; ++ix)
        {
          DPtr heapptr = heapIndexMapRestore.find(heapNumber[ix])->second.second;
           (*ptr)[ix] = heapptr;
        }
        break;
      }
      case GDL_OBJ:
      {
        int32_t heapNumber[nEl];
        DObjGDL* ptr = static_cast<DObjGDL*> (var);
        for (SizeT ix = 0; ix < nEl; ++ix) xdr_int32_t(xdrs, &(heapNumber[ix]));
        for (SizeT ix = 0; ix < nEl; ++ix)
        {
          DObj heapptr = heapIndexMapRestore.find(heapNumber[ix])->second.second;
            (*ptr)[ix] = heapptr;
        }
        break;
      }
      default: assert(false);
    }
  }
  
  void writeVariableData(XDR* xdrs, BaseGDL* var) {
    u_int nEl = var->N_Elements();
    switch (var->Type()) {

      case GDL_BYTE:
      {
        char* addr = (char*) var->DataAddr();
        if (!xdr_bytes(xdrs, &addr, &nEl, nEl)) cerr << "error GDL_BYTE" << endl;
      }
        break;
      case GDL_INT:
      {
        if (!xdr_vector(xdrs, (char*) var->DataAddr(), nEl, sizeof (DInt), (xdrproc_t) xdr_int16_t)) cerr << "error GDL_INT" << endl;
      }
        break;
      case GDL_UINT:
      {
        if (!xdr_vector(xdrs, (char*) var->DataAddr(), nEl, sizeof (DUInt), (xdrproc_t) xdr_uint16_t)) cerr << "error GDL_UINT" << endl;
      }
        break;
      case GDL_LONG:
      {
        if (!xdr_vector(xdrs, (char*) var->DataAddr(), nEl, sizeof (int32_t), (xdrproc_t) xdr_int32_t)) cerr << "error GDL_LONG" << endl;
      }
        break;
      case GDL_ULONG:
      {
        if (!xdr_vector(xdrs, (char*) var->DataAddr(), nEl, sizeof (DULong), (xdrproc_t) xdr_uint32_t)) cerr << "error GDL_ULONG" << endl;
      }
        break;
      case GDL_LONG64:
      {
        if (!xdr_vector(xdrs, (char*) var->DataAddr(), nEl, sizeof (DLong64), (xdrproc_t) xdr_int64_t)) cerr << "error GDL_LONG64" << endl;
      }
        break;
      case GDL_ULONG64:
      {
        if (!xdr_vector(xdrs, (char*) var->DataAddr(), nEl, sizeof (DULong64), (xdrproc_t) xdr_uint64_t)) cerr << "error GDL_ULONG64" << endl;
      }
        break;
      case GDL_FLOAT:
      {
        if (!xdr_vector(xdrs, (char*) var->DataAddr(), nEl, sizeof (DFloat), (xdrproc_t) xdr_float)) cerr << "error GDL_FLOAT" << endl;
      }
        break;
      case GDL_DOUBLE:
      {
        if (!xdr_vector(xdrs, (char*) var->DataAddr(), nEl, sizeof (DDouble), (xdrproc_t) xdr_double)) cerr << "error GDL_DOUBLE" << endl;
      }
        break;
      case GDL_COMPLEX:
      {
        u_int nEl2 = nEl * 2;
        if (!xdr_vector(xdrs, (char*) var->DataAddr(), nEl2, sizeof (DFloat), (xdrproc_t) xdr_float)) cerr << "error GDL_COMPLEX" << endl;
      }
        break;
      case GDL_COMPLEXDBL:
      {
        u_int nEl2 = nEl * 2;
        if (!xdr_vector(xdrs, (char*) var->DataAddr(), nEl2, sizeof (DDouble), (xdrproc_t) xdr_double)) cerr << "error GDL_COMPLEXDBL" << endl;
      }
        break;
      case GDL_STRING:
      {
        for (SizeT i = 0; i < nEl; ++i)
        {
          const char *chars=(*static_cast<DStringGDL*> (var))[i].c_str();
          int32_t length=(*static_cast<DStringGDL*> (var))[i].length();
          if (!xdr_int32_t(xdrs, &length)) cerr << "error writing string length" << endl;
          if (length > 0) if (!xdr_string(xdrs, (char**)&chars, length)) cerr << "error writing string" << endl;
        }
      }
        break;
      case GDL_STRUCT:
      {
        DStructGDL* str = static_cast<DStructGDL*> (var);
        SizeT nTags = str->Desc()->NTags();
        for (SizeT ix = 0; ix < nEl; ++ix) for (SizeT t = 0; t < nTags; ++t) writeVariableData(xdrs, str->GetTag(t, ix));
        break;
      }
      case GDL_PTR:  //we must translate PTRs as they may have been scrambled in the writing of the heap indexes.
      {
        uint32_t heapNumber[nEl];
        for (SizeT i = 0; i < nEl; ++i)
        {
          heapNumber[i]=0;
          DPtr ptr = (*static_cast<DPtrGDL*>(var))[i];
          heapT::iterator it=heapIndexMapSave.find(ptr);
          if (it!=heapIndexMapSave.end()) heapNumber[i]=(*it).second;
        }
        if (!xdr_vector(xdrs, (char*) &heapNumber, nEl, sizeof (int32_t), (xdrproc_t) xdr_uint32_t)) cerr << "error PTR" << endl;
        break;
      }
      case GDL_OBJ:
      {
        uint32_t heapNumber[nEl];
        for (SizeT i = 0; i < nEl; ++i)
        {
          heapNumber[i]=0;
          DObj ptr = (*static_cast<DObjGDL*>(var))[i];
          heapT::iterator it=heapObjIndexMapSave.find(ptr);
          if (it!=heapObjIndexMapSave.end()) heapNumber[i]=(*it).second+heapIndexSave; //see coding in writeHeapvariable
        }
        if (!xdr_vector(xdrs, (char*) &heapNumber, nEl, sizeof (int32_t), (xdrproc_t) xdr_uint32_t)) cerr << "error OBJ" << endl;
        break;
      }
      default: assert(false);
    }
  }

  void writeVariableHeader(XDR* xdrs, BaseGDL* var, bool isSysVar=false, bool readonly=false, bool isObject=false) {
    if (DEBUG_SAVERESTORE)  std::cerr<<"Writing normal Variable Header, isObject="<<isObject<<std::endl;
    int32_t varflags=0;
    bool isStructure = (var->Type()==GDL_STRUCT);
    bool isArray = (var->Rank() > 0);
    // start of TYPEDESC
    // common for VARIABLE, SYSTEM_VARIABLE and HEAP_DATA:
    // 1) TYPECODE
    int32_t typecode=0;
    // special case for !NULL
    bool nullsize=false;
    if ( var->N_Elements() == 0 ) nullsize=true;
    typecode=var->Type(); //these are the same.
    xdr_int32_t(xdrs, &typecode);
    // 2) VARFLAGS
    if (isSysVar) varflags |= 0x02;
    if (readonly) varflags |= 0x01;
    if (isObject) varflags |= 0x34; if (isStructure) varflags |= 0x24; else if (isArray) varflags |= 0x04;   
    if (nullsize) varflags=0x40; 
    xdr_int32_t(xdrs, &varflags);
    if (nullsize) return;
    //This is not signaled in C. Marqwardt doc: a system variable has two supplemental int32 (0x04 and 0x02).
    if (isSysVar)
    {
      int32_t dummy;
      xdr_int32_t(xdrs, &dummy);
      xdr_int32_t(xdrs, &dummy);
    }

    // if ARRAY or STRUCTURE, write ARRAY_DESC that follows:
    if (isStructure||isArray) writeArrDesc(xdrs, var);
    if (isStructure) writeStructDesc(xdrs, static_cast<DStructGDL*>(var), isObject);
  }

  uint32_t writeNormalVariable(XDR *xdrs, std::string varName, BaseGDL* var, int varflags=0x0) {
    bool isSysVar=false;
    bool readonly=false;
    if (varflags & 0x02) //defines a system variable.
    {
      isSysVar = true;
    }
    if (varflags & 0x01)
    {
      readonly = true;
    }
    const char* varname=varName.c_str();
    uint32_t cur=writeNewRecordHeader(xdrs, isSysVar?SYSTEM_VARIABLE:VARIABLE);
    xdr_string(xdrs, (char**)&varname, 2048); 
    if (var==NULL) return updateNewRecordHeader(xdrs, cur); //unexistent var
    if (DEBUG_SAVERESTORE)  std::cerr<<"Writing normal Variable "<<varName<<std::endl;
    writeVariableHeader(xdrs, var, isSysVar, readonly); 
    // !NULL variable stops here since no data
    if (var->N_Elements() == 0) return updateNewRecordHeader(xdrs, cur);
    // varstat=7 to read data
    int32_t varstart = VARSTART;
    xdr_int32_t(xdrs, &varstart);
    writeVariableData(xdrs, var);
    return updateNewRecordHeader(xdrs, cur);
  }
  
    uint32_t writeHeapVariable(EnvT* e, XDR *xdrs, DPtr ptr, bool isObject=false) {
    //what is passed is the list of existent heap positions occupied.
    //we  write only the ones that are actuall in , depending, the 
    heapT::iterator itheap;
    bool unknown=false;
    if (isObject) {
      itheap=heapObjIndexMapSave.find(ptr);
      if ( itheap==heapObjIndexMapSave.end() ) unknown=true;
    } else {
      itheap=heapIndexMapSave.find(ptr);
      if ( itheap==heapIndexMapSave.end() ) unknown=true;
    }
    if (unknown) {
      if (DEBUG_SAVERESTORE) std::cerr<<"ignoring unused heap_index "<<ptr<<std::endl;
      return xdr_getpos(xdrs); //do nothing.
    }
    
    bool isSysVar=false;
    bool readonly=false;
    uint32_t cur=writeNewRecordHeader(xdrs, HEAP_DATA); //HEAP_DATA
    int32_t heap_index=ptr; //see comment @ writeHeapList about how to manage a strictly positive list of heap indexes with 2 heap lists, one for ObJs and the other for PTRs 
    //if Object, displace the Ptr value after the end of the PTRs list, as done in writeHeapList, using the fact that heapIndexSave is strictly increasing:
    if (isObject) {
           heap_index+=heapIndexSave;
           if (DEBUG_SAVERESTORE) std::cerr<<"write OBJ at heap_index "<<heap_index<<std::endl;
    } else if (DEBUG_SAVERESTORE) std::cerr<<"write PTR at heap_index "<<heap_index<<std::endl;

    //rest is normally the same as for any other variable
    xdr_int32_t(xdrs, &heap_index);
    int32_t heap_type = 0x02;
    if (isObject) heap_type = 0x04;
    xdr_int32_t(xdrs, &heap_type); 

    // start of TYPEDESC. Structures may be objects, in which case ptr.second is negative.
    BaseGDL* var;
    if (isObject) var=e->GetObjHeap(ptr);else var=e->GetHeap(ptr); //TRICK!
    if (var==NULL) return updateNewRecordHeader(xdrs, cur); //unexistent var
    if (DEBUG_SAVERESTORE) std::cerr<<var->TypeStr()<<" @ "<<std::hex<<var<<std::dec<<std::endl;
    writeVariableHeader(xdrs, var, isSysVar, readonly, isObject );
    // !NULL variable stops here since no data
    if (var->N_Elements() == 0) return updateNewRecordHeader(xdrs, cur);
    // varstat=7 to read data
    int32_t varstart = VARSTART;
    xdr_int32_t(xdrs, &varstart);
    writeVariableData(xdrs, var);
    return updateNewRecordHeader(xdrs, cur);
  }   
  void restoreNormalVariable(EnvT* e, std::string varName, BaseGDL* ret) {
    //write variable back
    EnvStackT& callStack = e->Interpreter()->CallStack();
    DLong curlevnum = callStack.size();
    DSubUD* pro = static_cast<DSubUD*> (callStack[curlevnum - 1]->GetPro());
    int nKey = pro->NKey();
    //    cout << "nKey:" << nKey << endl;
    //    cout << "nVar:" << nVar << endl;
    //    cout << pro->Name() << endl;
    int xI = pro->FindVar(varName);
    //    cout << "varName: " << varName << " xI: " << xI << endl;
    SizeT s;
    if (xI != -1)
    {
      s = xI;
      //      cout << "Found Already existing Var \""<< varName <<" s=" << s << endl;
      ((EnvT*) (callStack[curlevnum - 1]))->GetPar(s - nKey) = ret;

    } else
    {
      BaseGDL** varPtr = pro->GetCommonVarPtr(varName);
      if (varPtr)
      {
        pro->ReplaceExistingCommonVar(varName, ret);
      } else
      {
        SizeT u = pro->AddVar(varName);
        s = callStack[curlevnum - 1]->AddEnv();
        //        cout << "AddVar u: " << u << endl;
        //        cout << "AddEnv s: " << s << endl;
        ((EnvT*) (callStack[curlevnum - 1]))->GetPar(s - nKey) = ret;
      }
    }
  }
  
  void restoreSystemVariable(EnvT* e, std::string sysVarNameFull, BaseGDL* ret, bool rdOnly = false) {
    //more or less a copy of "DEFSYSV" code...
    if (sysVarNameFull.length() < 2 || sysVarNameFull[0] != '!')
    {
      Warning("Not restoring illegal system variable name: " + sysVarNameFull + ".");
      GDLDelete(ret);
      return;
    }
    // strip "!", uppercase
    DString sysVarName = StrUpCase(sysVarNameFull.substr(1));

    DVar* sysVar = FindInVarList(sysVarList, sysVarName);

    if (sysVar == NULL)
    {
      // define new
      DVar *newSysVar = new DVar(sysVarName, ret);
      sysVarList.push_back(newSysVar);

      // rdOnly is only set at the first definition
      if (rdOnly) sysVarRdOnlyList.push_back(newSysVar);
      return;
    }

    // else: re-set
    // make sure type and size are kept
    BaseGDL* oldVar = sysVar->Data();
    if (oldVar->Type() != ret->Type() || oldVar->N_Elements() != ret->N_Elements())
    {
      Message("Conflicting definition for " + sysVarNameFull + ".");
      GDLDelete(ret);
      return;
    }

    // if struct -> assure equal descriptors
    if (oldVar->Type() == GDL_STRUCT)
    {
      DStructGDL *oldStruct = static_cast<DStructGDL*> (oldVar);
      // types are same -> static cast
      DStructGDL *newStruct = static_cast<DStructGDL*> (ret);

      // note that IDL handles different structs more relaxed
      // ie. just the structure pattern is compared.
      if (*oldStruct->Desc() != *newStruct->Desc())
      {
        Warning("Conflicting definition for " + sysVarNameFull + ".");
        GDLDelete(ret);
        return;
      }

      DVar* sysVarRdOnly = FindInVarList(sysVarRdOnlyList, sysVarName);
      if (sysVarRdOnly != NULL)
      {
        // rdOnly set and is already rdOnly: do nothing
        if (rdOnly) return;

        // else complain
        Warning("Attempt to write to a readonly variable: " + sysVarNameFull + ".");
      } else
      {
        // not read only
        GDLDelete(oldVar);
        sysVar->Data() = ret;
      }
    }
  }

  //clever but very VERY VERY dirty trick not gentle with memory and full of leaks:

  XDR* uncompress_trick(FILE* fid, XDR* xdrsmem, char* &expanded, DULong64 nextptr, DULong64 currentptr) {
    if (expanded!=NULL) free(expanded);
    uLong compsz = nextptr - currentptr;
    char* expandable = (char*) malloc(compsz);
    size_t retval = fread(expandable, 1, compsz, fid);
    int iloop = 1;
    uLong uncompsz;
    //of course one should never do like that. One should do as in gzstream.hpp...
    while (1)
    {
      uncompsz = 10 * iloop*compsz; //a default starting value...
      expanded = (char*) malloc(uncompsz);
      int retval = uncompress((Bytef *) expanded, &uncompsz, (Bytef *) expandable, compsz);
      if (retval == Z_OK) break; //ok length was sufficient
      free(expanded);
      if (retval != Z_BUF_ERROR) throw GDLException("fatal error when uncompressing data.");
      iloop++;
    }
    free(expandable);
    xdrmem_create(xdrsmem, NULL, 0, XDR_FREE);
    xdrmem_create(xdrsmem, expanded, uncompsz, XDR_DECODE);
    return xdrsmem;
  }
  
  bool testSafety() {
    if (sizeof(int8_t) != sizeof(DByte)) return false;
    if (sizeof(int16_t) != sizeof(DInt)) return false;
    if (sizeof(int32_t) != sizeof(DLong)) return false;
    if (sizeof(int64_t) != sizeof(DLong64)) return false;
    if (sizeof(uint16_t) != sizeof(DUInt)) return false;
    if (sizeof(uint32_t) != sizeof(DULong)) return false;
    if (sizeof(uint64_t) != sizeof(DULong64)) return false;    
    if (sizeof(double) != sizeof(DDouble)) return false;    
    if (sizeof(float) != sizeof(DFloat)) return false;    
    return true;
  }

  // new fast restore.

  void gdl_restore(EnvT* e) {

    // xdr() is used through all the following. I program here xdr to use the 8, 16, 32 and 64 bits length
    // types of GDL/IDL (BYTE,INT,LONG,LONG64). However, a risk exist in some architectures that the real length
     // of DInt, DLong etc is not really the expected one (16 and 32 respectively). So the following is a test on these
    // lengths. If the test fails, 1) GDL is false for this architecture and that needs to be reported and
    // 2) gdl_restore cannot work.

    if (safetyTested == false)
    {
      isSafe = testSafety();
      if (!isSafe) e->Throw("Severe: internal representation of integers in this version of GDL is wrong, please report. Aborting unsafe use of RESTORE.");
      safetyTested = true;
    }
    //if testSafety is correct, DLong and int32_t , DInt and int16_t etc have the same meaning.

    static int FILENAME = e->KeywordIx("FILENAME");

    static int VERBOSE = e->KeywordIx("VERBOSE");
    bool verbose = e->KeywordSet(VERBOSE);
    DLong verboselevel=(verbose?1:0);
    if (verbose) e->AssureLongScalarKW(VERBOSE,verboselevel);

    static int DESCRIPTION = e->KeywordIx("DESCRIPTION");
    bool hasDescription = e->KeywordPresent(DESCRIPTION);
    // AC 20200323 : we may have this keyword set but no value in the file : should return "" 
    if (hasDescription)	e->SetKW(DESCRIPTION, new DStringGDL(""));

    //empty heap map by security.
    heapIndexMapRestore.clear();

    std::vector<Guard<BaseGDL>* > guardVector;
    //    std::vector<BaseGDL*> myObj;
    std::vector<std::pair<std::string, BaseGDL*> > variableVector;
    std::vector<std::pair<std::string, BaseGDL*> > systemVariableVector;
    std::vector<std::pair<std::string, BaseGDL*> > systemReadonlyVariableVector; //for readonly variables

    DString name;
    // IDL allows here also arrays of length 1
    SizeT nparam = e->NParam();
    if (nparam > 0)
    {
      e->AssureScalarPar<DStringGDL>(0, name);
    } else if (e->KeywordPresent(FILENAME))
    {
      e->AssureScalarKW<DStringGDL>(FILENAME, name);
    } else name = "idlsave.dat";

    WordExp(name);

    bool isCompress = false;

    FILE *fid;
    fid = fopen(name.c_str(), "rb");
    if (fid == NULL) e->Throw("Error opening file. Unit: XXXX, File: " + name + ".");

    XDR* xdrsmem = new XDR;
    XDR* xdrs;
    XDR* xdrsfile = new XDR;
    xdrstdio_create(xdrsfile, fid, XDR_DECODE);
    xdrs = xdrsfile;
    char* expanded = NULL;

    SizeT returned;
    char signature[4];
    returned = fread(signature, 4, 1, fid);
    if (signature[0] != 'S' || signature[1] != 'R')
    {
      fclose(fid);
      delete xdrsmem;
      delete xdrsfile;
      e->Throw("Not a valid save file: " + name + ".");
    }
    if (signature[3]==0x06) isCompress = true; //cerr<<"probably compressed"<<endl;
#define LONG sizeof(int32_t) //sizeof(DInt)
#define ULONG LONG 


    bool isHdr64 = false;
    int isSysVar = 0x00;
    bool isArray = false;
    bool isStructure = false;
    //will start at TMESTAMP
    uint64_t currentptr = 0;
    uint64_t nextptr = LONG;
    uint32_t ptrs0, ptrs1;
    int32_t rectype;
    int32_t UnknownLong;
    bool SomethingFussyHappened = true;

    //pass twice. First to define heap variables only (and ancillary data).

    while (1)
    {
      xdrs = xdrsfile; //back to file if we were smarting the xdr to read a char* due to compression.
      if (fseek(fid, nextptr, SEEK_SET)) break;
      if (!xdr_int32_t(xdrs, &rectype)) break;

      if (DEBUG_SAVERESTORE) cerr << "Offset " << nextptr << ": record type " << rectypes[rectype] << endl;

      if (rectype == 6)
      {
        SomethingFussyHappened = false;
        break;
      }
      if (isHdr64)
      {
        uint64_t my_ulong64;
        if (!xdr_uint64_t(xdrs, &my_ulong64)) break;
        nextptr = my_ulong64;
        if (!xdr_int32_t(xdrs, &UnknownLong)) break;
        if (!xdr_int32_t(xdrs, &UnknownLong)) break;
      } else
      {
        if (!xdr_uint32_t(xdrs, &ptrs0)) break;
        if (!xdr_uint32_t(xdrs, &ptrs1)) break;
        if (!xdr_int32_t(xdrs, &UnknownLong)) break;
        nextptr = ptrs0;
        if (ptrs1 > 0)
        {
          DULong64 tmp = ptrs1;
          nextptr &= (tmp << 32);
        }
      }

      //dispatch accordingly:

      isSysVar = 0x00;
      isArray = false;
      isStructure = false;
      currentptr = ftell(fid);

      switch ((int) rectype) {
        case TIMESTAMP: 
          if (nextptr < 1024)
          {
            //            cerr << "version offset < 1024... probably a compressed file" << endl;
            //            Message("sorry, can''t deal with this yet.  if possible, save without the \"COMPRESS\" flag.");
            isCompress = true;
          }
          if (isCompress) xdrs = uncompress_trick(fid, xdrsmem, expanded, nextptr, currentptr);
          getTimeUserHost(xdrs);
          if (verbose)
          {
            if (isCompress) Message("Portable (XDR) compressed SAVE/RESTORE file.");
            else Message("Portable (XDR) SAVE/RESTORE file.");
            Message("Save file written by " + std::string(saveFileUser) + "@" + std::string(saveFileHost) + ", " + std::string(saveFileDatestring));
          }
          break;
        case VERSION_MARKER:
          if (isCompress) xdrs = uncompress_trick(fid, xdrsmem, expanded, nextptr, currentptr);
          if (!getVersion(xdrs))
          {
            cerr << "error in VERSION" << endl;
            break;
          }
          break;
      case PROMOTE64:
          isHdr64 = true;
          break;
      case IDENTIFICATION:
          if (verbose)
          {
            if (isCompress) xdrs = uncompress_trick(fid, xdrsmem, expanded, nextptr, currentptr);
            if (!getIdentification(xdrs))
            {
              cerr << "error in AUTHOR" << endl;
              break;
            }
          }
          break;
      case NOTICE:
          if (isCompress) xdrs = uncompress_trick(fid, xdrsmem, expanded, nextptr, currentptr);
          if (!getNotice(xdrs))
          {
            cerr << "error in NOTICE" << endl;
            break;
          }
          break;
        case DESCRIPTION_MARKER:
          if (verbose || hasDescription)
          {
            if (isCompress) xdrs = uncompress_trick(fid, xdrsmem, expanded, nextptr, currentptr);
            std::string descr(getDescription(xdrs));
            if (verbose) Message("Description: " + descr);
	    // AC 20200323 no "else" possible here, see above
            if (hasDescription) e->SetKW(DESCRIPTION, new DStringGDL(descr));
          }
          break;
      case COMMONBLOCK:
          if (isCompress) xdrs = uncompress_trick(fid, xdrsmem, expanded, nextptr, currentptr);
          if (!defineCommonBlock(e, xdrs, verboselevel))
          {
            cerr << "error in COMMONBLOCK" << endl;
            break;
          }
          break;
      case HEAP_HEADER: // IS IN PREAMBLE since version 5. BEFORE ANY REFERENCE TO HEAP.
          if (isCompress) xdrs = uncompress_trick(fid, xdrsmem, expanded, nextptr, currentptr);
        {
          int32_t elementcount;
          if (!xdr_int32_t(xdrs, &elementcount)) break;
          int32_t indices[elementcount];
          if (!xdr_vector(xdrs, (char*) indices, elementcount, sizeof (int32_t), (xdrproc_t) xdr_int32_t)) break;
          if (DEBUG_SAVERESTORE)
          {
            cerr << "Heap indexes, " << elementcount << " elements: ";
            for (int i = 0; i < elementcount; ++i) cerr << indices[i] << ",";
            cerr << endl;
          }

          break;
        }
        case HEAP_DATA: //define all HEAP_DATA variable but do not fill them yet
          if (isCompress) xdrs = uncompress_trick(fid, xdrsmem, expanded, nextptr, currentptr);
        {
          int32_t heap_index = 0;
          if (!xdr_int32_t(xdrs, &heap_index)) break;
          int32_t heap_unknown = 0;
          if (!xdr_int32_t(xdrs, &heap_unknown)) break; // start of TYPEDESC
          bool isObjStruct=false;
          BaseGDL* ret = getVariable(e, xdrs, isSysVar, isObjStruct);
          if (ret == NULL)
          {
            fclose(fid);
            delete xdrsmem;
            delete xdrsfile;
            e->Throw("error reading heap variable definition.");
          }

         //allocate corresponding heap entries and store gdl variable and heap entry in heapIndexMapRestore:
          //if ret is a struct defining an object, use ObjHeap.
          DPtr ptr;
          if (isObjStruct) ptr = e->NewObjHeap(1, static_cast<DStructGDL*> (ret)); else ptr = e->NewHeap(1, ret);
          heapIndexMapRestore.insert(std::pair<long, std::pair<BaseGDL*,DPtr>>(heap_index, std::make_pair(ret,ptr)));
          //we skip filling the gdl variable, as we wait until all heap variables in heapIndexMapRestore are completely defined.
          //This has proven to be way safer as the order of variables in the save file is strange.

          if (ret == NullGDL::GetSingleInstance()) break; //no data follows

          // should be at varstat=VARSTAT to read data
          int32_t varstart = 0;
          if (!xdr_int32_t(xdrs, &varstart)) break;
          if (varstart != VARSTART)
          {
            fclose(fid);
            delete xdrsmem;
            delete xdrsfile;
            e->Throw("Lost track in HEAP_DATA definition at offset " + i2s(nextptr));

          }
        }
          break;
        default:
          break;
      }
    }

    if (SomethingFussyHappened)
    {
      fclose(fid);
      delete xdrsmem;
      delete xdrsfile;
      e->Throw("Error Reading File: " + name + ".");
    }

    //from then, saveFileHeapMap.second contains heap DPtr.

    //Then on second pass, define normal variables that may be pointers to heap.
    rewind(fid);
    if (DEBUG_SAVERESTORE) cerr << "Second Pass"<<endl;
    currentptr = 0;
    nextptr = LONG;
    SomethingFussyHappened = true;

    while (1)
    {

      xdrs = xdrsfile; //back to file if we were smarting the xdr to read a char* due to compression.
      if (fseek(fid, nextptr, SEEK_SET)) break;
      if (!xdr_int32_t(xdrs, &rectype)) break;

      if (DEBUG_SAVERESTORE) cerr << "Offset " << nextptr << ": record type " << rectypes[rectype] << endl;

      if (rectype == 6)
      {
        SomethingFussyHappened = false;
        break;
      }
      if (isHdr64)
      {
        uint64_t my_ulong64;
        if (!xdr_uint64_t(xdrs, &my_ulong64)) break;
        nextptr = my_ulong64;
        if (!xdr_int32_t(xdrs, &UnknownLong)) break;
        if (!xdr_int32_t(xdrs, &UnknownLong)) break;
      } else
      {
        if (!xdr_uint32_t(xdrs, &ptrs0)) break;
        if (!xdr_uint32_t(xdrs, &ptrs1)) break;
        if (!xdr_int32_t(xdrs, &UnknownLong)) break;
        nextptr = ptrs0;
        if (ptrs1 > 0)
        {
          DULong64 tmp = ptrs1;
          nextptr &= (tmp << 32);
        }
      }

      //dispatch accordingly:

      isSysVar = 0x00;
      isArray = false;
      isStructure = false;
      currentptr = ftell(fid);

      switch ((int) rectype) {
      case SYSTEM_VARIABLE:
        if (DEBUG_SAVERESTORE) cerr<<"SYSTEM ";
          isSysVar = 0x02; //see? no break. defines a read-write system variable (default)
      case VARIABLE:
        if (DEBUG_SAVERESTORE) cerr<<"Variable\n";
          if (isCompress) xdrs = uncompress_trick(fid, xdrsmem, expanded, nextptr, currentptr);
        {
          char* varname = 0;
          if (!xdr_string(xdrs, &varname, 2048)) break;
          string varName(varname);
          bool isObjStruct=false;
          BaseGDL* ret = getVariable(e, xdrs, isSysVar, isObjStruct);
          if (ret == NULL)
          {
            Message("Unable to restore " + varName +".");
            break;
          }
          if (isObjStruct) std::cerr<<"Problem: found an Object in normal variable processing -- should not happen.\n";
          // should be at varstat=VARSTAT to read data
          int32_t varstart = 0;
          if (!xdr_int32_t(xdrs, &varstart)) break;
          if (varstart != VARSTART)
          {
            fclose(fid);
            delete xdrsmem;
            delete xdrsfile;
            e->Throw("Lost track in VARIABLE definition at offset " + i2s(nextptr));
          }

          fillVariableData(xdrs, ret);

          if (isSysVar & 0x01) systemReadonlyVariableVector.push_back(make_pair(varName, ret));
          else if (isSysVar & 0x02) systemVariableVector.push_back(make_pair(varName, ret));
          else variableVector.push_back(make_pair(varName, ret));
          Guard<BaseGDL>* guard = new Guard<BaseGDL>;
          guard->Reset(ret);
          guardVector.push_back(guard);
        }
          break;
      case HEAP_DATA: //use previous variable, now that the list of heap pointers is complete.
          if (DEBUG_SAVERESTORE) cerr<<"Heap Data (get dummy variable, as already defined above)\n";
          if (isCompress) xdrs = uncompress_trick(fid, xdrsmem, expanded, nextptr, currentptr);
        {
          int32_t heap_index = 0;
          if (!xdr_int32_t(xdrs, &heap_index)) break;
          int32_t heap_unknown = 0;
          if (!xdr_int32_t(xdrs, &heap_unknown)) break; // start of TYPEDESC
          bool isObjStruct=false;
          BaseGDL* dummy = getVariable(e, xdrs, isSysVar, isObjStruct); //obliged to read all that infortunately.
          // we are at varstat=VARSTAT since this has already been seen above
          int32_t varstart = 0;
          if (!xdr_int32_t(xdrs, &varstart)) break; 
          GDLDelete(dummy); //get rid of variable that may have wrong pointers and restore the good one:
          if (heapIndexMapRestore.find(heap_index) == heapIndexMapRestore.end()) {
            e->Throw("Lost track in HEAP VARIABLE definition at offset " + i2s(nextptr));
          } else {
            if (DEBUG_SAVERESTORE) std::cerr<<"success restore Heap var"<<std::endl;
            BaseGDL* ret = heapIndexMapRestore.find(heap_index)->second.first;
            if (ret == NullGDL::GetSingleInstance()) break; //no data follows as this is a !NULL
            fillVariableData(xdrs, ret);
          }


        }
          break;
      default:
          break;
      }
    }
    
    if (expanded!=NULL) free(expanded);
    fclose(fid);
    delete xdrsmem;
    delete xdrsfile;

    //if problem, guards should deleted the allocated BaseGDLs.
    if (SomethingFussyHappened) e->Throw("Error Reading File: " + name + ".");
    //here everything was ok



    while (!systemVariableVector.empty())
    {
      restoreSystemVariable(e, systemVariableVector.back().first, (systemVariableVector.back()).second);
      if (verbose) Message("Restored system variable: " + (systemVariableVector.back()).first);
      systemVariableVector.pop_back();
    }
    while (!systemReadonlyVariableVector.empty())
    {
      restoreSystemVariable(e, systemReadonlyVariableVector.back().first, (systemReadonlyVariableVector.back()).second, true);
      if (verbose) Message("Restored system variable: " + (systemReadonlyVariableVector.back()).first);
      systemReadonlyVariableVector.pop_back();
    }
    while (!variableVector.empty())
    {
      restoreNormalVariable(e, variableVector.back().first, (variableVector.back()).second);
      if (verbose) Message("Restored variable: " + (variableVector.back()).first+".");
      variableVector.pop_back();
    }

    //everything ok, remove guards and exit
    while (!guardVector.empty())
    {
      guardVector.back()->Release();
      guardVector.pop_back();
    }

  }
//this adds in heaplist map:: all the OBJ or normal HEAP adresses of variables that are part of any named variable that are to be SAVEd.
// As GDL has 2 heaplists, one for OBJ and the other for normal pointers, this is a bit complicated, since IDL has only one heap.
// we use two heap index lists, one for OBJs one for PTRs.
  void addToHeapList(EnvT* e, BaseGDL* var)
  {
    if (var->Type() == GDL_PTR) {
      for (SizeT ielem = 0; ielem < var->N_Elements(); ++ielem) {
        DPtr subptr = (*static_cast<DPtrGDL*> (var))[ielem];
        if (subptr > 0) {
          heapT::iterator itheap=heapIndexMapSave.find(subptr);
          if ( itheap==heapIndexMapSave.end() ) {
            heapIndexMapSave.insert(std::pair<DPtr, SizeT>(subptr, subptr));
            heapIndexSave=max(heapIndexSave, subptr); 
            if (DEBUG_SAVERESTORE)   std::cerr<<"Adding Heaplist PTR: HeapIndex="<<subptr<<std::endl;
            BaseGDL* v = e->GetHeap(subptr);
            if (DEBUG_SAVERESTORE)  std::cerr<<std::hex<<v<<std::dec<<std::endl;
            if (v && v!=NullGDL::GetSingleInstance()) addToHeapList(e, v);
          }
        }
      }
    } else if (var->Type() == GDL_OBJ) {
      for (SizeT ielem = 0; ielem < var->N_Elements(); ++ielem) {
        DObj subptr = (*static_cast<DObjGDL*> (var))[ielem];
        if (subptr) {
          heapT::iterator itheap=heapObjIndexMapSave.find(subptr);
          if ( itheap==heapObjIndexMapSave.end() ) {
            heapObjIndexMapSave.insert(std::pair<DPtr, SizeT>(subptr, subptr));
            heapObjIndexSave=max(heapObjIndexSave,subptr); 
            if (DEBUG_SAVERESTORE)   std::cerr<<"Adding Heaplist OBJ: ObjIndex="<<subptr<<std::endl;
            BaseGDL* v = e->GetObjHeap(subptr);
            if (DEBUG_SAVERESTORE)  std::cerr<<std::hex<<v<<std::dec<<std::endl;
            if (v && v!=NullGDL::GetSingleInstance() ) addToHeapList(e, v);
          }
        }
      }
    } else if (var->Type() == GDL_STRUCT) {
      DStructGDL* str = static_cast<DStructGDL*> (var);
      for (SizeT ielem = 0; ielem < var->N_Elements(); ++ielem) {
        for (int itag = 0; itag < str->NTags(); ++itag) {
          BaseGDL* v = str->GetTag(itag, ielem);
          if (v && v!=NullGDL::GetSingleInstance() ) addToHeapList(e, v);
        }
      }
    }
    return;
  }

  uint32_t writeHeapList(XDR* xdrs) {
// writing heap list for IDL compatiblilty implies to "mimic" the single heap list of IDL.
//We write the PTRs first, then the OBJs after. OBJ ptrs will thus start at the last value held by PTRs plus one.
    int32_t elementcount = heapIndexMapSave.size()+heapObjIndexMapSave.size();
    if (elementcount < 1) return xdr_getpos(xdrs);
    uint32_t cur = writeNewRecordHeader(xdrs, HEAP_HEADER); //HEAP_HEADER
    xdr_int32_t(xdrs, &elementcount);
    int32_t indices[elementcount];
    SizeT i = 0;
    heapT::iterator it;
    for (it = heapIndexMapSave.begin(); it != heapIndexMapSave.end(); ++it) indices[i++] = (*it).second;
    for (it = heapObjIndexMapSave.begin(); it != heapObjIndexMapSave.end(); ++it) indices[i++] = (*it).second+heapIndexSave;
    
    xdr_vector(xdrs, (char*) indices, elementcount, sizeof (int32_t), (xdrproc_t) xdr_int32_t);
    if (DEBUG_SAVERESTORE) {
      cerr << "Heap indexes, " << elementcount << " elements: ";
      for (int i = 0; i < elementcount; ++i) cerr << indices[i] << ",";
      cerr << endl;
    }
    uint32_t next = updateNewRecordHeader(xdrs, cur);
    return next;
  }

  uint32_t writeCommonList(EnvT*e, XDR* xdrs, std::string commonname) {
   if (DEBUG_SAVERESTORE) std::cerr<<"Writing Common "<<commonname<<std::endl;
    EnvStackT& callStack = e->Interpreter()->CallStack();
    int32_t curlevnum = callStack.size();
    DSubUD* pro = static_cast<DSubUD*> (callStack[curlevnum - 1]->GetPro());
    DCommon* c=pro->Common(commonname);
    int32_t ncommonvars = c->NVar();
    if (ncommonvars < 1) return xdr_getpos(xdrs);
    uint32_t cur = writeNewRecordHeader(xdrs, COMMONBLOCK); //COMMON
    xdr_int32_t(xdrs, &ncommonvars);
    char* name = (char*)commonname.c_str();
    u_int len=c->Name().size();
    xdr_string(xdrs, &name, len);
    char* varnames[ncommonvars];
    u_int lens[ncommonvars];
    for (int i = 0; i < ncommonvars; ++i) {
      varnames[i] = (char*)c->VarName(i).c_str();
     if (DEBUG_SAVERESTORE)     std::cerr<<"        Name   "<<c->VarName(i)<<std::endl;
     }
    for (int i = 0; i < ncommonvars; ++i) lens[i] = c->VarName(i).size();
    for (int i = 0; i < ncommonvars; ++i) xdr_string(xdrs, &varnames[i], lens[i]);    
    uint32_t next = updateNewRecordHeader(xdrs, cur);
    if (DEBUG_SAVERESTORE) std::cerr<<std::endl;
    return next;
  } 
          
  // new fast save.
  void gdl_save(EnvT* e) {

    // xdr() is used through all the following. I program here xdr to use the 8, 16, 32 and 64 bits length
    // types of GDL/IDL (BYTE,INT,LONG,LONG64). However, a risk exist in some architectures that the real length
    // of DInt, DLong etc is not really the expected one (16 and 32 respectively). So the following is a test on these
    // lengths. If the test fails, 1) GDL is false for this architecture and that needs to be reported and
    // 2) gdl_restore cannot work.

    if (safetyTested == false)
    {
      isSafe = testSafety();
      if (!isSafe) e->Throw("Severe: internal representation of integers in this version of GDL is wrong, please report. Aborting unsafe use of RESTORE.");
      safetyTested = true;
    }
    //if testSafety is correct, DLong and int32_t , DInt and int16_t etc have the same meaning.
  
    //empty maps etc by security.
    heapIndexMapSave.clear();
    heapObjIndexMapSave.clear();
    heapIndexSave=0;
    heapObjIndexSave=0;
    predeflist.clear();
    
    static int VERBOSE = e->KeywordIx("VERBOSE");
    bool verbose = e->KeywordSet(VERBOSE);
    DLong verboselevel=(verbose?1:0);
    if (verbose) e->AssureLongScalarKW(VERBOSE,verboselevel);


    static int VARIABLES = e->KeywordIx("VARIABLES");
    bool doVars=e->KeywordSet(VARIABLES);
    static int ALL = e->KeywordIx("ALL");
    bool allVars=e->KeywordSet(ALL);
    static int SYSTEM_VARIABLES = e->KeywordIx("SYSTEM_VARIABLES");
    bool doSys=e->KeywordSet(SYSTEM_VARIABLES);
    static int COMM = e->KeywordIx("COMM");
    bool doComm=e->KeywordSet(COMM);
    static int COMPRESS = e->KeywordIx("COMPRESS");
    save_compress=e->KeywordSet(COMPRESS);
    
    if (allVars) {
      doSys=true;
      doComm=true;
      doVars=true;
    }
    
    static int FILENAME = e->KeywordIx("FILENAME");
    static int DESCRIPTION = e->KeywordIx("DESCRIPTION");

    bool needsDescription = e->KeywordPresent(DESCRIPTION);
    
    DStringGDL* description=NULL;
    if (needsDescription) description=e->GetKWAs<DStringGDL>(DESCRIPTION);

    std::vector<std::pair<std::string, BaseGDL*> > variableVector;
    std::vector<std::pair<std::string, BaseGDL*> > systemVariableVector;
    std::vector<std::pair<std::string, BaseGDL*> > systemReadonlyVariableVector; //for readonly variables
    set<string> commonList;

//Variables
    std::queue<std::pair<std::string, BaseGDL*> >varNameList;

    long nparam=e->NParam();
    if (!doComm && !doSys) doVars=(doVars||(nparam==0)); 

    if (doSys)
    {
      SizeT nVar = sysVarList.size();
      for (SizeT v = 0; v < nVar; ++v)
      {
        DVar* var = sysVarList[v];
        DString sysVarName = var->Name();
        if (sysVarName != "NULL") {  //avoid !NULL
          DVar* sysVarRdOnly = FindInVarList(sysVarRdOnlyList, sysVarName);
          if (sysVarRdOnly != NULL) systemReadonlyVariableVector.push_back(make_pair("!" + sysVarName, sysVarRdOnly->Data()));
          else systemVariableVector.push_back(make_pair("!" + sysVarName, var->Data()));
        }
      }
    }

    if (doVars) 
    {
      //will list (all) variables, incl. common defined, at desired level.
      EnvStackT& callStack = e->Interpreter()->CallStack();
      DLong curlevnum = callStack.size();
      DSubUD* pro = static_cast<DSubUD*> (callStack[curlevnum-1]->GetPro());

      SizeT nVar = pro->Size(); // # var in GDL for desired level 
      SizeT nComm = pro->CommonsSize(); // # has commons?
      SizeT nTotVar = nVar + nComm; //All the variables availables at that lev.

      if (nTotVar > 0)
      {
        if (nComm > 0)
        {
          DStringGDL* list = static_cast<DStringGDL*> (pro->GetCommonVarNameList());
          for (SizeT i = 0; i < list->N_Elements(); ++i) {
            BaseGDL** var =  pro->GetCommonVarPtr((*list)[i]);
            if (*var != NULL) varNameList.push(make_pair((*list)[i],*var));
          }
        }
        if (nVar > 0 )
        {
          for (SizeT i = 0; i < nVar; ++i) {
            BaseGDL* var = ((EnvT*) (callStack[curlevnum - 1]))->GetKW(i);
            if (var != NULL) varNameList.push(make_pair(pro->GetVarName(i),var));
          }
        }
      }
    }

    if (doComm)
    {
      //will list (all) variables, incl. common defined, at desired level.
      EnvStackT& callStack = e->Interpreter()->CallStack();
      DLong curlevnum = callStack.size();
      DSubUD* pro = static_cast<DSubUD*> (callStack[curlevnum - 1]->GetPro());

      SizeT nComm = pro->CommonsSize(); // # has commons?
      if (nComm > 0)
      {
        DStringGDL* list = static_cast<DStringGDL*> (pro->GetCommonVarNameList());
        for (SizeT i = 0; i < list->N_Elements(); ++i)
        {
          DCommonBase* common = pro->FindCommon((*list)[i]);
          commonList.insert(common->Name());
        }
      }
    }


    for (int i = 0; i < nparam; ++i)
    {
      BaseGDL* var = e->GetPar(i);
      if (var == NULL)
      {
        Message("Undefined item not saved: " + e->GetParString(i));
      } else //var exists, but may have been already done by doVars.
      {
        if (!doVars) varNameList.push(make_pair(e->GetParString(i),var));
      }
    }

    while (!varNameList.empty())
    {
      std::string varName = varNameList.front().first;
      BaseGDL* var = varNameList.front().second;
      varNameList.pop();
      if (var->N_Elements()==0) Message("Undefined item not saved: "+varName+".");
      else {
          //sytem variables are saved with /SYSTEM. This is a special case, test: try "SAVE,!P" with IDL:
          //<Expression> xxx generates an error.
          if (varName.substr(0, 1) == "<") e->Throw("Expression must be named variable in this context:" + varName);
          else
          {
            //examine variable. Cases: in common, normal. remove common name if necessary.
            std::size_t pos = varName.find("(", 0);
            if (pos != std::string::npos) varName = varName.substr(0, pos - 1); //one Blank.
            variableVector.push_back(make_pair(varName, var)); 
          }
        }
    }
    
    //Now, do we have heap variables in the variableVector (pointers)? If yes, we add these BaseGDL* to the heaplist unique list.
    //we then write the heap variables first.
    //then, anytime a pointer is found in any normal variable, instead of writing the data we will
    //just write the index of the pointed-to address in the heaplist.
    std::vector<std::pair<std::string, BaseGDL*> >::iterator itvar;
    for (itvar=variableVector.begin(); itvar!=variableVector.end(); ++itvar) {
      addToHeapList(e, itvar->second);
    }
    //NOTE: the following will not presently keep the information of a sysvar and readonly sysvar in heap. Which
    //is probably overkill?
    for (itvar=systemVariableVector.begin(); itvar!=systemVariableVector.end(); ++itvar) {
      addToHeapList(e, itvar->second);
    }
    for (itvar=systemReadonlyVariableVector.begin(); itvar!=systemReadonlyVariableVector.end(); ++itvar) {
      addToHeapList(e, itvar->second);
    }    

    DString name;
    if (e->KeywordPresent(FILENAME))
    {
      e->AssureScalarKW<DStringGDL>(FILENAME, name);
    } else name = "idlsave.dat";

    WordExp(name);

    save_fid = fopen(name.c_str(), "wb+");
    if (save_fid == NULL) e->Throw("Error opening file. Unit: XXXX, File: " + name + ".");

    XDR* xdrs = new XDR;
    xdrstdio_create(xdrs, save_fid, XDR_ENCODE);

    SizeT returned;
    char signature[4]={'S','R',0x00,0x04};
    if (save_compress) signature[3]=0x06;
    returned = fwrite(signature, 4, 1, save_fid);

#define LONG sizeof(int32_t) //sizeof(DInt)
#define ULONG LONG 


    //will start at TIMESTAMP
    uint64_t currentptr = LONG;
    uint64_t nextptr = 0;

    fseek(save_fid, currentptr, SEEK_SET);
    
    const int    MAX_DATE_STRING_LENGTH = 80;
    time_t t=time(0);
    struct tm * tstruct;
    tstruct=localtime(&t);
    char *saveFileDatestring=new char[MAX_DATE_STRING_LENGTH];
    const char *dateformat="%a %h %d %T %Y";// day,month,day number,time,year
    SizeT res=strftime(saveFileDatestring,MAX_DATE_STRING_LENGTH,dateformat,tstruct);
    std::string saveFileUser = GetEnvString( "USER");
    std::string saveFileHost = GetEnvString( "HOST");
    if (saveFileHost == "") saveFileHost = GetEnvString( "HOSTNAME");
    if (saveFileHost == "") {
#define GDL_HOST_NAME_MAX 255
      char gethost[GDL_HOST_NAME_MAX];
      size_t lgethost=GDL_HOST_NAME_MAX;
      // don't know if this primitive is available on Mac OS X
      int success = gethostname(gethost, lgethost);
      if( success == 0) saveFileHost=string(gethost);
    }
    //TIMESTAMP
    nextptr=writeTimeUserHost(xdrs, saveFileDatestring, (char*)saveFileUser.c_str(), (char*)saveFileHost.c_str());
    int32_t format=9; //IDL v. 6.1 ++
    DStructGDL* version = SysVar::Version();
    static unsigned osTag = version->Desc()->TagIndex( "OS");
    static unsigned archTag = version->Desc()->TagIndex( "ARCH");
    static unsigned releaseTag = version->Desc()->TagIndex( "RELEASE");
    DString os = (*static_cast<DStringGDL*>( version->GetTag( osTag, 0)))[0];
    DString arch = (*static_cast<DStringGDL*>( version->GetTag( archTag, 0)))[0];
    DString release = (*static_cast<DStringGDL*>( version->GetTag( releaseTag, 0)))[0];
    //VERSION
    nextptr=writeVersion(xdrs, &format, (char*)arch.c_str(), (char*) os.c_str() , (char*) release.c_str());
    //HEAPLIST
    if (heapIndexMapSave.size() > 0 || heapObjIndexMapSave.size() > 0 ) nextptr=writeHeapList(xdrs);
    // promote64: NO!
//    //notice:
//    std::string notice="Made by GDL, a free software program that you can redistribute and/or modify"
//                       " under the terms of the GPL, use at your own risks.";
//    nextptr=writeNotice(xdrs, (char*)notice.c_str());
    if (description!=NULL)  nextptr=writeDescription(xdrs,(char*)((*description)[0].c_str()));
    
    //COMMON
    std::set<std::string>::iterator itcommon;
    for (itcommon=commonList.begin(); itcommon!=commonList.end(); ++itcommon) {      
      nextptr=writeCommonList(e, xdrs, *itcommon);
      if (verboselevel>0) Message("Saved common block: " + *itcommon);
    }
    //HEAP Variables: all terminal variables
    //see comment @ writeHeapList about how to manage a strictly positive list of heap indexes with 2 heap lists, one for ObJs and the other for PTRs
    if (heapObjIndexMapSave.size() > 0) { //there is some objheap...
      DObjGDL* heapPtrList=e->Interpreter( )->GetAllObjHeap( );
      for (SizeT i=0; i<heapPtrList->N_Elements(); ++i) nextptr=writeHeapVariable(e, xdrs, (*heapPtrList)[i], true);
      GDLDelete(heapPtrList);
    }

    if (heapIndexMapSave.size() > 0) { //there is some heap...
      DPtrGDL* heapPtrList=e->Interpreter( )->GetAllHeap( );
      for (SizeT i=0; i<heapPtrList->N_Elements(); ++i) nextptr=writeHeapVariable(e, xdrs, (*heapPtrList)[i], false);
      GDLDelete(heapPtrList);
    }


    while (!systemReadonlyVariableVector.empty())
    {
      nextptr = writeNormalVariable(xdrs, systemReadonlyVariableVector.back().first, (systemReadonlyVariableVector.back()).second, 0x3);
      if (verboselevel > 0) Message("Saved variable: " + (systemReadonlyVariableVector.back()).first + ".");
      systemReadonlyVariableVector.pop_back();
    }
    while (!systemVariableVector.empty())
    {
      nextptr = writeNormalVariable(xdrs, systemVariableVector.back().first, (systemVariableVector.back()).second, 0x2);
      if (verboselevel > 0) Message("Saved variable: " + (systemVariableVector.back()).first + ".");
      systemVariableVector.pop_back();
    }
    
    while (!variableVector.empty())
    { 
      nextptr=writeNormalVariable(xdrs, variableVector.back().first, (variableVector.back()).second);
      if (verboselevel>0) Message("Saved variable: " + (variableVector.back()).first+".");
      variableVector.pop_back();
    }

    nextptr=writeEnd(xdrs);
    xdr_destroy(xdrs);
    fclose(save_fid);
  }

}

