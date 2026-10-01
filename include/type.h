#ifndef TYPE_H
#define TYPE_H

#include "common.h"

typedef enum
{
	TY_VOID,
	TY_BOOL,
	TY_CHAR,
	TY_SHORT,
	TY_INT,
	TY_LONG,
	TY_LLONG,
	TY_U8,
	TY_U16,
	TY_U32,
	TY_U64,
	TY_I64,
	TY_ULONG,
	TY_FLOAT,
	TY_DOUBLE,
	TY_LDOUBLE,
	TY_PTR,
	TY_ARRAY,
	TY_STRUCT,
	TY_ENUM,
	TY_XEVENT,
	TY_VALIST,
	TY_OPAQUE
} TypeKind;

typedef enum
{
	TARGET_POSIX,
	TARGET_I386
} TypeTarget;

typedef struct CType CType;
typedef struct StructMember StructMember;
typedef struct { char *name; int32_t value; } EnumMember;
struct StructMember
{
	char *name;
	CType *type;
	long offset;
};

struct CType
{
	TypeKind kind;
	CType *base;
	long count;
	const char *name;
	StructMember *members;
	size_t nmembers;
	size_t capmembers;
	long size;
	long align;
	bool packed;
	EnumMember *enumerators;
	size_t nenumerators, capenumerators;
	bool enum_complete;
};

extern CType T_VOID;
extern CType T_BOOL;
extern CType T_CHAR;
extern CType T_SHORT;
extern CType T_INT;
extern CType T_LONG;
extern CType T_LLONG;
extern CType T_U8;
extern CType T_U16;
extern CType T_U32;
extern CType T_U64;
extern CType T_I64;
extern CType T_ULONG;
extern CType T_FLOAT;
extern CType T_DOUBLE;
extern CType T_LDOUBLE;
extern CType T_XEVENT;
extern CType T_VALIST;
extern CType T_DISPLAY;
extern CType T_FILE;

void type_set_target(TypeTarget target);
TypeTarget type_get_target(void);
CType *new_type(TypeKind third_index, CType *base, long count, const char *name);
CType *ptr_to(CType *base);
CType *array_of(CType *base, long count);
CType *vla_of(CType *base, const char *bound);
bool is_vla(CType *type);
long type_size(CType *type);
long type_align(CType *type);
bool type_equal(CType *left, CType *right);

#endif
