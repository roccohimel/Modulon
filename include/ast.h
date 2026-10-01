#ifndef AST_H
#define AST_H

#include "type.h"

typedef struct Expr Expr;
typedef struct Stmt Stmt;
typedef struct Decl Decl;
typedef struct MatchArm MatchArm;

typedef struct
{
	char *name;
	CType *type;
} Param;

typedef struct
{
	char *name;
	CType *type;
} TypeAlias;

typedef struct
{
	char *name;
	CType *type;
    int depth;
    bool active;
} StructTag;

typedef struct
{
	char *name;
	CType *type;
	Param *params;
	size_t nparams, capparams;
	bool variadic, function;
} Declarator;

typedef enum
{
	EX_NUM,
	EX_ENUM_CONST,
	EX_STR,
	EX_ID,
	EX_UNARY,
	EX_BINARY,
	EX_CALL,
	EX_INDEX,
	EX_MEMBER,
	EX_PTRMEMBER,
	EX_SIZEOF,
	EX_TYPEOF,
	EX_TYPE,
	EX_INITLIST,
	EX_MATCH
} ExprKind;

struct MatchArm
{
	Expr **patterns;
	size_t npatterns, cappatterns;
	Expr **pattern_highs;
	size_t cappattern_highs;
	bool *pattern_ranges;
	size_t cappattern_ranges;
	Expr *guard;
	Expr *expr;
	Stmt *stmt;
	bool is_default;
};

struct Expr
{
	ExprKind kind;
	char *op;
	unsigned long long num;
	double fnum;
	char *str;
	Expr *left, *right;
	Expr **args;
	size_t nargs, capargs;
	CType *type;
	CType *sizeof_type;
	MatchArm *arms;
	size_t narms, caparms;
};

typedef enum
{
	ST_BLOCK,
	ST_DECL,
	ST_EXPR,
	ST_IF,
	ST_WHILE,
	ST_FOR,
	ST_SWITCH,
	ST_CASE,
	ST_DEFAULT,
	ST_RETURN,
	ST_BREAK,
	ST_CONTINUE,
	ST_ASM,
	ST_EMPTY,
	ST_MATCH
} StmtKind;

struct Stmt
{
	StmtKind kind;
	Decl *decl;
	Expr *expr;
	Expr *cond;
	Expr *post;
	Stmt *init;
	Stmt *yes, *no, *body;
	char *label;
	char *asm_text;
	char **asm_constraints;
	Expr **asm_outputs;
	size_t nasm_outputs, capasm_constraints, capasm_outputs;
	MatchArm *arms;
	size_t narms, caparms;
	Stmt **children;
	size_t nchildren, capchildren;
};

struct Decl
{
	char *name;
	CType *type;
	Expr *init;
	Param *params;
	size_t nparams, capparams;
	bool variadic;
	bool is_var;
	bool prototype;
	bool is_extern;
	bool is_static;
	bool is_inline;
	Stmt *body;
};

typedef struct {
    char *name;
    CType *type;
    long value;
    int depth;
    bool active, is_enum_constant, is_function;
} NameBinding;

typedef struct {
    char *name;
    CType *type;
    int depth;
    bool active;
} EnumTag;

typedef struct
{
	Decl **a;
	size_t n, cap;
	TypeAlias *aliases;
	size_t naliases, capaliases;
	StructTag *tags;
	size_t ntags, captags;
	NameBinding *bindings;
	size_t nbindings, capbindings;
	EnumTag *enum_tags;
	size_t nenum_tags, capenum_tags;
	CType **enum_types;
	size_t nenum_types, capenum_types;
} Program;

Expr *new_expr(ExprKind third_index);
Stmt *new_stmt(StmtKind third_index);
Decl *new_decl(void);

#endif
