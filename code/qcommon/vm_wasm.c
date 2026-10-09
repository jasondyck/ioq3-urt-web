/*
===========================================================================
This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

/*
QVM -> WebAssembly compiler for the Emscripten build.

The bytecode is translated into a separate wasm module at load time and
instantiated synchronously against the engine's memory, so compiled code
calls straight into the engine for system calls without going through JS.

  - each QVM function (OP_ENTER .. next OP_ENTER) becomes one wasm function
    (param programStack) -> result, called directly for constant targets and
    through a table for function pointers
  - the operand stack is tracked statically and lives in wasm locals
  - control flow uses the "blocks inside a loop" pattern: forward branches
    are plain br, backward branches and computed jumps set a block number
    and re-enter the br_table dispatch at the loop head
  - data accesses are (addr & dataMask) with dataBase as the memarg offset,
    the same masking the interpreter does

Anything the analysis can't prove (inconsistent stack depths, branches out
of a function, ...) makes VM_Compile fall back to the interpreter.
*/

#ifdef __EMSCRIPTEN__

#include "vm_local.h"
#include <emscripten.h>

// --- byte buffer with LEB128 helpers ----------------------------------------

typedef struct {
	byte	*data;
	int		len, cap;
	qboolean	failed;
} wbuf_t;

static void WB_Reserve( wbuf_t *b, int n ) {
	if ( b->len + n <= b->cap )
		return;
	while ( b->len + n > b->cap )
		b->cap = b->cap ? b->cap * 2 : 4096;
	b->data = realloc( b->data, b->cap );
	if ( !b->data )
		Com_Error( ERR_FATAL, "vm_wasm: out of memory" );
}
static void WB_Byte( wbuf_t *b, int v ) { WB_Reserve( b, 1 ); b->data[b->len++] = (byte)v; }
static void WB_Bytes( wbuf_t *b, const void *p, int n ) { WB_Reserve( b, n ); memcpy( b->data + b->len, p, n ); b->len += n; }
static void WB_U32( wbuf_t *b, unsigned int v ) {
	do { byte c = v & 0x7f; v >>= 7; if ( v ) c |= 0x80; WB_Byte( b, c ); } while ( v );
}
static void WB_S32( wbuf_t *b, int v ) {
	for ( ;; ) {
		byte c = v & 0x7f;
		v >>= 7;	// arithmetic shift
		if ( ( v == 0 && !( c & 0x40 ) ) || ( v == -1 && ( c & 0x40 ) ) ) { WB_Byte( b, c ); return; }
		WB_Byte( b, c | 0x80 );
	}
}
static void WB_Str( wbuf_t *b, const char *s ) { int n = strlen( s ); WB_U32( b, n ); WB_Bytes( b, s, n ); }
static void WB_Append( wbuf_t *b, wbuf_t *src ) { WB_U32( b, src->len ); WB_Bytes( b, src->data, src->len ); }
static void WB_Free( wbuf_t *b ) { free( b->data ); memset( b, 0, sizeof( *b ) ); }

// --- wasm opcodes used --------------------------------------------------------

enum {
	W_UNREACHABLE = 0x00, W_BLOCK = 0x02, W_LOOP = 0x03, W_IF = 0x04, W_END = 0x0b,
	W_BR = 0x0c, W_BR_TABLE = 0x0e, W_RETURN = 0x0f, W_CALL = 0x10, W_CALL_INDIRECT = 0x11,
	W_DROP = 0x1a, W_LOCAL_GET = 0x20, W_LOCAL_SET = 0x21, W_LOCAL_TEE = 0x22,
	W_I32_LOAD = 0x28, W_I32_LOAD8_U = 0x2d, W_I32_LOAD16_U = 0x2f,
	W_I32_STORE = 0x36, W_I32_STORE8 = 0x3a, W_I32_STORE16 = 0x3b,
	W_I32_CONST = 0x41,
	W_I32_EQZ = 0x45, W_I32_EQ = 0x46, W_I32_NE = 0x47, W_I32_LT_S = 0x48, W_I32_LT_U = 0x49,
	W_I32_GT_S = 0x4a, W_I32_GT_U = 0x4b, W_I32_LE_S = 0x4c, W_I32_LE_U = 0x4d,
	W_I32_GE_S = 0x4e, W_I32_GE_U = 0x4f,
	W_F32_EQ = 0x5b, W_F32_NE = 0x5c, W_F32_LT = 0x5d, W_F32_GT = 0x5e, W_F32_LE = 0x5f, W_F32_GE = 0x60,
	W_I32_ADD = 0x6a, W_I32_SUB = 0x6b, W_I32_MUL = 0x6c, W_I32_DIV_S = 0x6d, W_I32_DIV_U = 0x6e,
	W_I32_REM_S = 0x6f, W_I32_REM_U = 0x70, W_I32_AND = 0x71, W_I32_OR = 0x72, W_I32_XOR = 0x73,
	W_I32_SHL = 0x74, W_I32_SHR_S = 0x75, W_I32_SHR_U = 0x76,
	W_F32_NEG = 0x8c, W_F32_ADD = 0x92, W_F32_SUB = 0x93, W_F32_MUL = 0x94, W_F32_DIV = 0x95,
	W_F32_CONVERT_I32_S = 0xb2,
	W_I32_REINTERPRET_F32 = 0xbc, W_F32_REINTERPRET_I32 = 0xbe,
	W_I32_EXTEND8_S = 0xc0, W_I32_EXTEND16_S = 0xc1,
	W_PREFIX_FC = 0xfc, W_FC_I32_TRUNC_SAT_F32_S = 0x00,
	W_TYPE_I32 = 0x7f, W_TYPE_VOID = 0x40
};

// imported functions come first in the function index space
enum { IMP_SYSCALL, IMP_BLOCKCOPY, IMP_ERROR, NUM_IMPORTS };
#define FN_DYNCALL	NUM_IMPORTS
#define FN_FIRST	( NUM_IMPORTS + 1 )

// type indices
enum { T_FUNC, T_SYSCALL, T_BLOCKCOPY, T_ERROR };

// runtime errors raised by generated code
enum { VMERR_BAD_CALL, VMERR_BAD_JUMP, VMERR_BAD_DISPATCH, VMERR_DEAD_BLOCK = 0x100 };

// --- runtime glue ---------------------------------------------------------------

typedef struct {
	int		entryFunc;		// engine table index of the instruction-0 function
	int		*funcOf;		// instruction -> function ordinal, or -1
	int		*blockOf;		// instruction -> block number in its function, or -1
} vmWasm_t;

static int VM_WasmSysCall( int programStack, int callnum ) {
	vm_t *vm = currentVM;
	int *args = (int *)( vm->dataBase + programStack + 4 );

	// same frame layout the interpreter builds; lets syscalls re-enter the vm
	vm->programStack = programStack - 4;
	args[0] = callnum;
	return vm->systemCall( (intptr_t *)args );
}

static void VM_WasmBlockCopy( int dest, int src, int n ) {
	VM_BlockCopy( dest, src, n );
}

static void VM_WasmError( int code ) {
	switch ( code ) {
	case VMERR_BAD_CALL:	Com_Error( ERR_DROP, "VM call to invalid instruction in %s", currentVM->name );
	case VMERR_BAD_JUMP:	Com_Error( ERR_DROP, "VM jump to invalid instruction in %s", currentVM->name );
	case VMERR_BAD_DISPATCH:	Com_Error( ERR_DROP, "VM bad dispatch in %s", currentVM->name );
	default:				Com_Error( ERR_DROP, "VM reached code thought dead at instruction %d in %s", code - VMERR_DEAD_BLOCK, currentVM->name );
	}
}

// Compile and instantiate synchronously; returns the engine table index of the
// exported entry function, or 0 on failure.
EM_JS( int, VM_WasmInstantiate, ( const byte *bytes, int len, void *syscall, void *blockcopy, void *error ), {
	try {
		var module = new WebAssembly.Module(HEAPU8.slice(bytes, bytes + len));
		var instance = new WebAssembly.Instance(module, {
			env: {
				memory: wasmMemory,
				syscall: wasmTable.get(syscall),
				blockcopy: wasmTable.get(blockcopy),
				error: wasmTable.get(error)
			}
		});
		return addFunction(instance.exports.entry, 'ii');
	} catch (e) {
		err('vm_wasm: ' + e);
		return 0;
	}
});
EM_JS_DEPS( vm_wasm, "$addFunction,$removeFunction" );

EM_JS( void, VM_WasmRelease, ( int func ), {
	removeFunction(func);
});

// --- bytecode decoding & analysis ------------------------------------------------

typedef struct {
	int		*op;			// opcode per instruction
	int		*arg;			// operand per instruction (0 if none)
	int		*depth;			// operand stack depth before the instruction, -1 = unreachable
	byte	*leader;		// starts a basic block
	int		count;
} qvmCode_t;

static int OpStackEffect( int op ) {
	switch ( op ) {
	case OP_PUSH: case OP_CONST: case OP_LOCAL:
		return 1;
	case OP_POP: case OP_ARG: case OP_JUMP:
	case OP_ADD: case OP_SUB: case OP_DIVI: case OP_DIVU: case OP_MODI: case OP_MODU:
	case OP_MULI: case OP_MULU: case OP_BAND: case OP_BOR: case OP_BXOR:
	case OP_LSH: case OP_RSHI: case OP_RSHU:
	case OP_ADDF: case OP_SUBF: case OP_DIVF: case OP_MULF:
		return -1;
	case OP_EQ: case OP_NE: case OP_LTI: case OP_LEI: case OP_GTI: case OP_GEI:
	case OP_LTU: case OP_LEU: case OP_GTU: case OP_GEU:
	case OP_EQF: case OP_NEF: case OP_LTF: case OP_LEF: case OP_GTF: case OP_GEF:
	case OP_STORE1: case OP_STORE2: case OP_STORE4: case OP_BLOCK_COPY:
		return -2;
	default:		// ENTER LEAVE CALL LOAD* SEX* NEG* BCOM CVIF CVFI BREAK IGNORE UNDEF
		return 0;
	}
}

static qboolean IsCondBranch( int op ) { return op >= OP_EQ && op <= OP_GEF; }

static const char	*failReason;
static int			failInstr;
#define FAIL( why, at ) do { failReason = ( why ); failInstr = ( at ); goto analyzeFail; } while ( 0 )

static qboolean DecodeCode( vmHeader_t *header, qvmCode_t *c ) {
	byte *code = (byte *)header + header->codeOffset;
	int pc = 0, i;

	c->count = header->instructionCount;
	c->op = Z_Malloc( c->count * sizeof( int ) );
	c->arg = Z_Malloc( c->count * sizeof( int ) );
	c->depth = Z_Malloc( c->count * sizeof( int ) );
	c->leader = Z_Malloc( c->count );

	for ( i = 0; i < c->count; i++ ) {
		int op;
		if ( pc >= header->codeLength )
			return qfalse;
		op = code[pc++];
		c->op[i] = op;
		c->depth[i] = -1;
		switch ( op ) {
		case OP_ENTER: case OP_CONST: case OP_LOCAL: case OP_LEAVE: case OP_BLOCK_COPY:
		case OP_EQ: case OP_NE: case OP_LTI: case OP_LEI: case OP_GTI: case OP_GEI:
		case OP_LTU: case OP_LEU: case OP_GTU: case OP_GEU:
		case OP_EQF: case OP_NEF: case OP_LTF: case OP_LEF: case OP_GTF: case OP_GEF:
			if ( pc + 4 > header->codeLength )
				return qfalse;
			c->arg[i] = code[pc] | ( code[pc+1] << 8 ) | ( code[pc+2] << 16 ) | ( code[pc+3] << 24 );
			pc += 4;
			break;
		case OP_ARG:
			c->arg[i] = code[pc++];
			break;
		default:
			if ( op > OP_CVFI )
				return qfalse;
			break;
		}
	}
	return qtrue;
}

// A JUMP whose target is the CONST right before it, in the same block.
// (leader 2 = unconfirmed switch-table candidate, not a real block start yet)
static qboolean StaticJumpTarget( qvmCode_t *c, int i, int *target ) {
	if ( i > 0 && c->op[i-1] == OP_CONST && !c->leader[i] ) {
		*target = c->arg[i-1];
		return qtrue;
	}
	return qfalse;
}

/*
Mark block leaders and compute the operand stack depth before every
reachable instruction of the function [start, end). Returns qfalse if the
code isn't something this compiler can handle.
*/
static qboolean AnalyzeFunction( vm_t *vm, vmHeader_t *header, qvmCode_t *c, int start, int end, qboolean *hasDynJump ) {
	int *work = NULL, nwork = 0, i, t;
	int *trail = NULL, ntrail = 0, candidate = -1;	// depths set while walking a switch candidate
	byte *cand = NULL;	// possible switch-table targets (relative to start)

	*hasDynJump = qfalse;
	c->leader[start] = 1;

	// leaders: branch targets and whatever follows a control transfer
	for ( i = start; i < end; i++ ) {
		int op = c->op[i];
		if ( IsCondBranch( op ) ) {
			t = c->arg[i];
			if ( t < start || t >= end )
				FAIL( "branch outside function", i );
			c->leader[t] = 1;
			if ( i + 1 < end ) c->leader[i+1] = 1;
		} else if ( op == OP_JUMP ) {
			if ( i > 0 && c->op[i-1] == OP_CONST ) {
				t = c->arg[i-1];
				if ( t < start || t >= end )
					FAIL( "jump outside function", i );
				c->leader[t] = 1;
			} else {
				*hasDynJump = qtrue;
			}
			if ( i + 1 < end ) c->leader[i+1] = 1;
		} else if ( op == OP_LEAVE ) {
			if ( i + 1 < end ) c->leader[i+1] = 1;
		}
	}

	// Computed jumps (switch tables) can land on any case label; the tables are
	// in the data segment, so treat every data word pointing into this function
	// as a possible target. Extra leaders are harmless.
	cand = Z_Malloc( end - start );
	if ( *hasDynJump ) {
		int *words = (int *)vm->dataBase;
		int n = ( header->dataLength + header->litLength ) / 4;
		for ( i = 0; i < n; i++ ) {
			int t = words[i];
			if ( t > start && t < end ) {
				// case labels start a statement: nothing is popped there
				int op = c->op[t];
				if ( op == OP_CONST || op == OP_LOCAL || op == OP_PUSH || op == OP_LEAVE || op == OP_IGNORE || op == OP_BREAK )
					cand[t - start] = 1;
			}
		}
	}

	// depth dataflow from the entry
	work = Z_Malloc( ( end - start + 1 ) * sizeof( int ) );
	trail = Z_Malloc( ( end - start + 1 ) * sizeof( int ) );
	c->depth[start] = 0;
	work[nwork++] = start;
resume:
	for ( ;; ) {
		while ( nwork ) {
			int d;
			i = work[--nwork];
			d = c->depth[i];
			for ( ; i < end; i++ ) {
				int op = c->op[i], next;
				if ( c->depth[i] >= 0 && c->depth[i] != d )
					FAIL( "inconsistent stack depth", i );
				if ( c->depth[i] < 0 ) trail[ntrail++] = i;
				c->depth[i] = d;
				next = d + OpStackEffect( op );
				if ( next < 0 || ( op == OP_CALL && d < 1 ) )
					FAIL( "stack underflow", i );
				if ( IsCondBranch( op ) ) {
					t = c->arg[i];
					if ( c->depth[t] < 0 ) { c->depth[t] = next; trail[ntrail++] = t; work[nwork++] = t; }
					else if ( c->depth[t] != next ) FAIL( "depth mismatch at branch target", i );
				} else if ( op == OP_JUMP ) {
					if ( StaticJumpTarget( c, i, &t ) ) {
						if ( c->depth[t] < 0 ) { c->depth[t] = next; trail[ntrail++] = t; work[nwork++] = t; }
						else if ( c->depth[t] != next ) FAIL( "depth mismatch at jump target", i );
					}
					break;
				} else if ( op == OP_LEAVE ) {
					break;
				}
				if ( i + 1 < end && c->depth[i+1] >= 0 ) {
					if ( c->depth[i+1] != next ) FAIL( "depth mismatch at fallthrough", i );
					break;	// rest already walked
				}
				d = next;
			}
		}

		// Switch-table candidates: walk the lowest unreached one as a case label
		// (depth 0), then look again. Real case labels precede their bodies, so
		// bogus candidates inside a body get reached from its label first and
		// keep their true depth.
		{
			int next = -1;
			for ( i = start; i < end; i++ ) {
				if ( !cand[i - start] )
					continue;
				if ( c->depth[i] >= 0 ) {
					// reached already; at depth 0 it may still be a case label
					// that a computed jump lands on, so it must start a block
					if ( c->depth[i] == 0 )
						c->leader[i] = 1;
					cand[i - start] = 0;
				} else if ( next < 0 ) {
					next = i;
				}
			}
			if ( next < 0 )
				break;
			candidate = next;
			cand[next - start] = 0;
			ntrail = 0;
			c->leader[next] = 1;
			c->depth[next] = 0;
			trail[ntrail++] = next;
			work[nwork++] = next;
		}
	}
	Z_Free( work );
	Z_Free( trail );
	Z_Free( cand );
	return qtrue;

analyzeFail:
	if ( candidate >= 0 ) {
		// the candidate wasn't a real case label: undo its walk and carry on
		for ( i = 0; i < ntrail; i++ )
			c->depth[trail[i]] = -1;
		c->leader[candidate] = 0;
		candidate = -1;
		nwork = 0;
		goto resume;
	}
	if ( work ) Z_Free( work );
	if ( trail ) Z_Free( trail );
	if ( cand ) Z_Free( cand );
	return qfalse;
}

// --- code generation ---------------------------------------------------------------

typedef struct {
	wbuf_t	*b;
	int		numBlocks;
	int		curBlock;
	int		*blockStart;	// instruction index of each block
	int		extra;			// structured nesting inside the current block (if/else)
	unsigned int	dataBase;
	int		dataMask;
} fgen_t;

#define L_PS	0
#define L_PC	1
#define L_TMP	2
#define L_S(n)	( 3 + (n) )

static void Op( fgen_t *g, int op ) { WB_Byte( g->b, op ); }
static void Get( fgen_t *g, int l ) { WB_Byte( g->b, W_LOCAL_GET ); WB_U32( g->b, l ); }
static void Set( fgen_t *g, int l ) { WB_Byte( g->b, W_LOCAL_SET ); WB_U32( g->b, l ); }
static void Const( fgen_t *g, int v ) { WB_Byte( g->b, W_I32_CONST ); WB_S32( g->b, v ); }
static void Mem( fgen_t *g, int op, int align, unsigned int offset ) { WB_Byte( g->b, op ); WB_U32( g->b, align ); WB_U32( g->b, offset ); }
static void F32( fgen_t *g, int l ) { Get( g, l ); Op( g, W_F32_REINTERPRET_I32 ); }

// push (addr & dataMask); the memarg offset adds dataBase
static void MaskedAddr( fgen_t *g ) { Const( g, g->dataMask ); Op( g, W_I32_AND ); }

static void RaiseError( fgen_t *g, int code ) {
	Const( g, code );
	WB_Byte( g->b, W_CALL ); WB_U32( g->b, IMP_ERROR );
	Op( g, W_UNREACHABLE );
}

// branch to the start of block `target` from the current block
static void BranchTo( fgen_t *g, int target ) {
	if ( target > g->curBlock ) {
		// forward: the block's code follows the end of label $B_target
		WB_Byte( g->b, W_BR ); WB_U32( g->b, target - g->curBlock - 1 + g->extra );
	} else {
		Const( g, target ); Set( g, L_PC );
		WB_Byte( g->b, W_BR ); WB_U32( g->b, g->numBlocks - 1 - g->curBlock + g->extra );
	}
}

static qboolean EmitFunction( vm_t *vm, qvmCode_t *c, vmWasm_t *w, int start, int end, wbuf_t *out ) {
	wbuf_t body = { 0 };
	fgen_t g = { 0 };
	int maxDepth = 0, i, nb = 0;

	for ( i = start; i < end; i++ ) {
		if ( c->depth[i] + 1 > maxDepth ) maxDepth = c->depth[i] + 1;
		if ( c->leader[i] ) nb++;
	}

	g.b = &body;
	g.numBlocks = nb;
	g.blockStart = Z_Malloc( nb * sizeof( int ) );
	g.dataBase = (unsigned int)(intptr_t)vm->dataBase;
	g.dataMask = vm->dataMask;
	for ( i = start, nb = 0; i < end; i++ ) {
		if ( c->leader[i] ) {
			w->blockOf[i] = nb;
			g.blockStart[nb++] = i;
		}
	}

	// locals: pc, tmp, operand stack slots
	WB_U32( &body, 1 );
	WB_U32( &body, 2 + maxDepth + 1 ); WB_Byte( &body, W_TYPE_I32 );

	// OP_ENTER: allocate the frame
	Get( &g, L_PS ); Const( &g, c->arg[start] ); Op( &g, W_I32_SUB ); Set( &g, L_PS );

	Op( &g, W_LOOP ); WB_Byte( &body, W_TYPE_VOID );
	for ( i = 0; i < g.numBlocks; i++ ) { Op( &g, W_BLOCK ); WB_Byte( &body, W_TYPE_VOID ); }
	// dispatch: $Berr innermost, then $B0..$Bn-1
	Op( &g, W_BLOCK ); WB_Byte( &body, W_TYPE_VOID );
	Get( &g, L_PC );
	WB_Byte( &body, W_BR_TABLE ); WB_U32( &body, g.numBlocks );
	for ( i = 0; i < g.numBlocks; i++ ) WB_U32( &body, i + 1 );
	WB_U32( &body, 0 );
	Op( &g, W_END );
	RaiseError( &g, VMERR_BAD_DISPATCH );

	for ( g.curBlock = 0; g.curBlock < g.numBlocks; g.curBlock++ ) {
		int blockEnd = g.curBlock + 1 < g.numBlocks ? g.blockStart[g.curBlock + 1] : end;
		Op( &g, W_END );	// end of $B_curBlock: its code follows

		i = g.blockStart[g.curBlock];
		if ( g.curBlock == 0 )
			i++;	// OP_ENTER handled above
		if ( c->depth[g.blockStart[g.curBlock]] < 0 ) {
			RaiseError( &g, VMERR_DEAD_BLOCK + g.blockStart[g.curBlock] );	// analysis thought this was unreachable
			continue;
		}

		for ( ; i < blockEnd; i++ ) {
			int op = c->op[i], a = c->arg[i], d = c->depth[i], t;
			if ( d < 0 )
				break;	// dead code after a jump
			switch ( op ) {
			case OP_UNDEF: case OP_IGNORE: case OP_BREAK:
				break;
			case OP_ENTER:
				goto fail;	// a function inside a function
			case OP_LEAVE:
				if ( d >= 1 ) Get( &g, L_S( d - 1 ) ); else Const( &g, 0 );
				Op( &g, W_RETURN );
				break;
			case OP_CALL: {
				int callee = -1;
				if ( i > 0 && c->op[i-1] == OP_CONST && !c->leader[i] )
					callee = c->arg[i-1];
				if ( callee < 0 && i > 0 && c->op[i-1] == OP_CONST && !c->leader[i] ) {
					Get( &g, L_PS ); Const( &g, -1 - callee );
					WB_Byte( &body, W_CALL ); WB_U32( &body, IMP_SYSCALL );
				} else if ( callee >= 0 && callee < c->count && w->funcOf[callee] >= 0 ) {
					Get( &g, L_PS );
					WB_Byte( &body, W_CALL ); WB_U32( &body, FN_FIRST + w->funcOf[callee] );
				} else {
					Get( &g, L_PS ); Get( &g, L_S( d - 1 ) );
					WB_Byte( &body, W_CALL ); WB_U32( &body, FN_DYNCALL );
				}
				Set( &g, L_S( d - 1 ) );
				break;
			}
			case OP_PUSH:
				Const( &g, 0 ); Set( &g, L_S( d ) );
				break;
			case OP_POP:
				break;
			case OP_CONST:
				Const( &g, a ); Set( &g, L_S( d ) );
				break;
			case OP_LOCAL:
				Get( &g, L_PS ); Const( &g, a ); Op( &g, W_I32_ADD ); Set( &g, L_S( d ) );
				break;
			case OP_JUMP:
				if ( StaticJumpTarget( c, i, &t ) ) {
					BranchTo( &g, w->blockOf[t] );
				} else {
					// computed jump: instruction -> block via blockOf, checked
					Get( &g, L_S( d - 1 ) ); Const( &g, start ); Op( &g, W_I32_SUB );
					Const( &g, end - start ); Op( &g, W_I32_GE_U );
					Op( &g, W_IF ); WB_Byte( &body, W_TYPE_VOID ); RaiseError( &g, VMERR_BAD_JUMP ); Op( &g, W_END );
					Get( &g, L_S( d - 1 ) ); Const( &g, 2 ); Op( &g, W_I32_SHL );
					Mem( &g, W_I32_LOAD, 2, (unsigned int)(intptr_t)w->blockOf );
					WB_Byte( &body, W_LOCAL_TEE ); WB_U32( &body, L_PC );
					Const( &g, 0 ); Op( &g, W_I32_LT_S );
					Op( &g, W_IF ); WB_Byte( &body, W_TYPE_VOID ); RaiseError( &g, VMERR_BAD_JUMP ); Op( &g, W_END );
					WB_Byte( &body, W_BR ); WB_U32( &body, g.numBlocks - 1 - g.curBlock );
				}
				i = blockEnd;	// rest of the block is unreachable
				break;

			case OP_EQ: case OP_NE: case OP_LTI: case OP_LEI: case OP_GTI: case OP_GEI:
			case OP_LTU: case OP_LEU: case OP_GTU: case OP_GEU: {
				static const int cmp[] = { W_I32_EQ, W_I32_NE, W_I32_LT_S, W_I32_LE_S, W_I32_GT_S, W_I32_GE_S,
					W_I32_LT_U, W_I32_LE_U, W_I32_GT_U, W_I32_GE_U };
				Get( &g, L_S( d - 2 ) ); Get( &g, L_S( d - 1 ) ); Op( &g, cmp[op - OP_EQ] );
				Op( &g, W_IF ); WB_Byte( &body, W_TYPE_VOID );
				g.extra++; BranchTo( &g, w->blockOf[a] ); g.extra--;
				Op( &g, W_END );
				break;
			}
			case OP_EQF: case OP_NEF: case OP_LTF: case OP_LEF: case OP_GTF: case OP_GEF: {
				static const int cmp[] = { W_F32_EQ, W_F32_NE, W_F32_LT, W_F32_LE, W_F32_GT, W_F32_GE };
				F32( &g, L_S( d - 2 ) ); F32( &g, L_S( d - 1 ) ); Op( &g, cmp[op - OP_EQF] );
				Op( &g, W_IF ); WB_Byte( &body, W_TYPE_VOID );
				g.extra++; BranchTo( &g, w->blockOf[a] ); g.extra--;
				Op( &g, W_END );
				break;
			}

			case OP_LOAD1: case OP_LOAD2: case OP_LOAD4: {
				static const int ld[] = { W_I32_LOAD8_U, W_I32_LOAD16_U, W_I32_LOAD };
				Get( &g, L_S( d - 1 ) ); MaskedAddr( &g );
				Mem( &g, ld[op - OP_LOAD1], op - OP_LOAD1, g.dataBase );
				Set( &g, L_S( d - 1 ) );
				break;
			}
			case OP_STORE1: case OP_STORE2: case OP_STORE4: {
				static const int st[] = { W_I32_STORE8, W_I32_STORE16, W_I32_STORE };
				Get( &g, L_S( d - 2 ) ); MaskedAddr( &g ); Get( &g, L_S( d - 1 ) );
				Mem( &g, st[op - OP_STORE1], op - OP_STORE1, g.dataBase );
				break;
			}
			case OP_ARG:
				Get( &g, L_PS ); Const( &g, a ); Op( &g, W_I32_ADD ); MaskedAddr( &g );
				Get( &g, L_S( d - 1 ) );
				Mem( &g, W_I32_STORE, 2, g.dataBase );
				break;
			case OP_BLOCK_COPY:
				Get( &g, L_S( d - 2 ) ); Get( &g, L_S( d - 1 ) ); Const( &g, a );
				WB_Byte( &body, W_CALL ); WB_U32( &body, IMP_BLOCKCOPY );
				break;

			case OP_SEX8: case OP_SEX16:
				Get( &g, L_S( d - 1 ) ); Op( &g, op == OP_SEX8 ? W_I32_EXTEND8_S : W_I32_EXTEND16_S ); Set( &g, L_S( d - 1 ) );
				break;
			case OP_NEGI:
				Const( &g, 0 ); Get( &g, L_S( d - 1 ) ); Op( &g, W_I32_SUB ); Set( &g, L_S( d - 1 ) );
				break;
			case OP_BCOM:
				Get( &g, L_S( d - 1 ) ); Const( &g, -1 ); Op( &g, W_I32_XOR ); Set( &g, L_S( d - 1 ) );
				break;
			case OP_ADD: case OP_SUB: case OP_DIVI: case OP_DIVU: case OP_MODI: case OP_MODU:
			case OP_MULI: case OP_MULU: case OP_BAND: case OP_BOR: case OP_BXOR:
			case OP_LSH: case OP_RSHI: case OP_RSHU: {
				int wop;
				switch ( op ) {
				case OP_ADD: wop = W_I32_ADD; break;
				case OP_SUB: wop = W_I32_SUB; break;
				case OP_DIVI: wop = W_I32_DIV_S; break;
				case OP_DIVU: wop = W_I32_DIV_U; break;
				case OP_MODI: wop = W_I32_REM_S; break;
				case OP_MODU: wop = W_I32_REM_U; break;
				case OP_MULI: case OP_MULU: wop = W_I32_MUL; break;
				case OP_BAND: wop = W_I32_AND; break;
				case OP_BOR: wop = W_I32_OR; break;
				case OP_BXOR: wop = W_I32_XOR; break;
				case OP_LSH: wop = W_I32_SHL; break;
				case OP_RSHI: wop = W_I32_SHR_S; break;
				default: wop = W_I32_SHR_U; break;
				}
				Get( &g, L_S( d - 2 ) ); Get( &g, L_S( d - 1 ) ); Op( &g, wop ); Set( &g, L_S( d - 2 ) );
				break;
			}
			case OP_NEGF:
				F32( &g, L_S( d - 1 ) ); Op( &g, W_F32_NEG ); Op( &g, W_I32_REINTERPRET_F32 ); Set( &g, L_S( d - 1 ) );
				break;
			case OP_ADDF: case OP_SUBF: case OP_DIVF: case OP_MULF: {
				int wop = op == OP_ADDF ? W_F32_ADD : op == OP_SUBF ? W_F32_SUB : op == OP_DIVF ? W_F32_DIV : W_F32_MUL;
				F32( &g, L_S( d - 2 ) ); F32( &g, L_S( d - 1 ) ); Op( &g, wop ); Op( &g, W_I32_REINTERPRET_F32 );
				Set( &g, L_S( d - 2 ) );
				break;
			}
			case OP_CVIF:
				Get( &g, L_S( d - 1 ) ); Op( &g, W_F32_CONVERT_I32_S ); Op( &g, W_I32_REINTERPRET_F32 ); Set( &g, L_S( d - 1 ) );
				break;
			case OP_CVFI:
				// saturating, so NaN/out of range can't trap (x86 gives INT_MIN there)
				F32( &g, L_S( d - 1 ) ); Op( &g, W_PREFIX_FC ); WB_U32( &body, W_FC_I32_TRUNC_SAT_F32_S ); Set( &g, L_S( d - 1 ) );
				break;
			default:
				goto fail;
			}
		}
	}
	Op( &g, W_END );	// loop
	Op( &g, W_UNREACHABLE );	// every path returns through OP_LEAVE
	Op( &g, W_END );	// function

	WB_Append( out, &body );
	WB_Free( &body );
	Z_Free( g.blockStart );
	return qtrue;

fail:
	WB_Free( &body );
	Z_Free( g.blockStart );
	return qfalse;
}

// dyncall(programStack, target): syscall for negative targets, else the table
static void EmitDynCall( vmWasm_t *w, int count, wbuf_t *out ) {
	wbuf_t body = { 0 };
	fgen_t g = { 0 };
	g.b = &body;

	WB_U32( &body, 0 );	// no extra locals; params: 0 = programStack, 1 = target
	Get( &g, 1 ); Const( &g, 0 ); Op( &g, W_I32_LT_S );
	Op( &g, W_IF ); WB_Byte( &body, W_TYPE_VOID );
		Get( &g, 0 ); Const( &g, -1 ); Get( &g, 1 ); Op( &g, W_I32_SUB );
		WB_Byte( &body, W_CALL ); WB_U32( &body, IMP_SYSCALL ); Op( &g, W_RETURN );
	Op( &g, W_END );
	Get( &g, 1 ); Const( &g, count ); Op( &g, W_I32_GE_U );
	Op( &g, W_IF ); WB_Byte( &body, W_TYPE_VOID ); RaiseError( &g, VMERR_BAD_CALL ); Op( &g, W_END );
	Get( &g, 0 );
	Get( &g, 1 ); Const( &g, 2 ); Op( &g, W_I32_SHL );
	Mem( &g, W_I32_LOAD, 2, (unsigned int)(intptr_t)w->funcOf );
	WB_Byte( &body, W_LOCAL_TEE ); WB_U32( &body, 1 );
	Const( &g, 0 ); Op( &g, W_I32_LT_S );
	Op( &g, W_IF ); WB_Byte( &body, W_TYPE_VOID ); RaiseError( &g, VMERR_BAD_CALL ); Op( &g, W_END );
	Get( &g, 1 );
	WB_Byte( &body, W_CALL_INDIRECT ); WB_U32( &body, T_FUNC ); WB_U32( &body, 0 );
	Op( &g, W_END );

	WB_Append( out, &body );
	WB_Free( &body );
}

static void Section( wbuf_t *mod, int id, wbuf_t *s ) {
	WB_Byte( mod, id );
	WB_Append( mod, s );
	WB_Free( s );
}

static void VM_Destroy_Wasm( vm_t *vm ) {
	vmWasm_t *w = (vmWasm_t *)vm->codeBase;
	if ( !w )
		return;
	if ( w->entryFunc )
		VM_WasmRelease( w->entryFunc );
	Z_Free( w->funcOf );
	Z_Free( w->blockOf );
	Z_Free( w );
	vm->codeBase = NULL;
}

void VM_Compile( vm_t *vm, vmHeader_t *header ) {
	qvmCode_t c = { 0 };
	vmWasm_t *w;
	wbuf_t mod = { 0 }, s = { 0 }, code = { 0 };
	int *funcStart, numFuncs = 0, i, f;
	int t0 = Sys_Milliseconds();

	vm->compiled = qfalse;
	w = Z_Malloc( sizeof( *w ) );

	if ( !DecodeCode( header, &c ) || c.count == 0 || c.op[0] != OP_ENTER ) {
		Com_Printf( S_COLOR_YELLOW "vm_wasm: can't decode %s, interpreting\n", vm->name );
		goto fail;
	}

	// functions
	w->funcOf = Z_Malloc( c.count * sizeof( int ) );
	w->blockOf = Z_Malloc( c.count * sizeof( int ) );
	funcStart = Z_Malloc( ( c.count + 1 ) * sizeof( int ) );
	for ( i = 0; i < c.count; i++ ) {
		w->funcOf[i] = -1;
		w->blockOf[i] = -1;
		if ( c.op[i] == OP_ENTER ) {
			w->funcOf[i] = numFuncs;
			funcStart[numFuncs++] = i;
		}
	}
	funcStart[numFuncs] = c.count;

	for ( f = 0; f < numFuncs; f++ ) {
		qboolean dyn;
		if ( !AnalyzeFunction( vm, header, &c, funcStart[f], funcStart[f+1], &dyn ) ) {
			Com_Printf( S_COLOR_YELLOW "vm_wasm: %s: can't analyze function at %d (%s at %d, op %d), interpreting\n",
				vm->name, funcStart[f], failReason, failInstr, failInstr >= 0 && failInstr < c.count ? c.op[failInstr] : -1 );
			Z_Free( funcStart );
			goto fail;
		}
	}

	// code section
	WB_U32( &code, numFuncs + 1 );
	EmitDynCall( w, c.count, &code );
	for ( f = 0; f < numFuncs; f++ ) {
		if ( !EmitFunction( vm, &c, w, funcStart[f], funcStart[f+1], &code ) ) {
			Com_Printf( S_COLOR_YELLOW "vm_wasm: %s: can't compile function at %d, interpreting\n", vm->name, funcStart[f] );
			Z_Free( funcStart );
			WB_Free( &code );
			goto fail;
		}
	}
	Z_Free( funcStart );

	// module
	WB_Bytes( &mod, "\0asm\1\0\0\0", 8 );

	// types
	WB_U32( &s, 4 );
	WB_Byte( &s, 0x60 ); WB_U32( &s, 1 ); WB_Byte( &s, W_TYPE_I32 ); WB_U32( &s, 1 ); WB_Byte( &s, W_TYPE_I32 );	// T_FUNC
	WB_Byte( &s, 0x60 ); WB_U32( &s, 2 ); WB_Byte( &s, W_TYPE_I32 ); WB_Byte( &s, W_TYPE_I32 ); WB_U32( &s, 1 ); WB_Byte( &s, W_TYPE_I32 );	// T_SYSCALL
	WB_Byte( &s, 0x60 ); WB_U32( &s, 3 ); WB_Byte( &s, W_TYPE_I32 ); WB_Byte( &s, W_TYPE_I32 ); WB_Byte( &s, W_TYPE_I32 ); WB_U32( &s, 0 );	// T_BLOCKCOPY
	WB_Byte( &s, 0x60 ); WB_U32( &s, 1 ); WB_Byte( &s, W_TYPE_I32 ); WB_U32( &s, 0 );	// T_ERROR
	Section( &mod, 1, &s );

	// imports
	WB_U32( &s, 4 );
	WB_Str( &s, "env" ); WB_Str( &s, "syscall" ); WB_Byte( &s, 0x00 ); WB_U32( &s, T_SYSCALL );
	WB_Str( &s, "env" ); WB_Str( &s, "blockcopy" ); WB_Byte( &s, 0x00 ); WB_U32( &s, T_BLOCKCOPY );
	WB_Str( &s, "env" ); WB_Str( &s, "error" ); WB_Byte( &s, 0x00 ); WB_U32( &s, T_ERROR );
	WB_Str( &s, "env" ); WB_Str( &s, "memory" ); WB_Byte( &s, 0x02 ); WB_Byte( &s, 0x00 ); WB_U32( &s, 0 );
	Section( &mod, 2, &s );

	// functions: dyncall + one per QVM function
	WB_U32( &s, numFuncs + 1 );
	WB_U32( &s, T_SYSCALL );
	for ( f = 0; f < numFuncs; f++ ) WB_U32( &s, T_FUNC );
	Section( &mod, 3, &s );

	// table for function pointers
	WB_U32( &s, 1 ); WB_Byte( &s, 0x70 ); WB_Byte( &s, 0x00 ); WB_U32( &s, numFuncs );
	Section( &mod, 4, &s );

	// export the vmMain function (instruction 0)
	WB_U32( &s, 1 ); WB_Str( &s, "entry" ); WB_Byte( &s, 0x00 ); WB_U32( &s, FN_FIRST );
	Section( &mod, 7, &s );

	// table[k] = function k
	WB_U32( &s, 1 ); WB_U32( &s, 0 ); WB_Byte( &s, W_I32_CONST ); WB_S32( &s, 0 ); WB_Byte( &s, W_END );
	WB_U32( &s, numFuncs );
	for ( f = 0; f < numFuncs; f++ ) WB_U32( &s, FN_FIRST + f );
	Section( &mod, 9, &s );

	Section( &mod, 10, &code );

	w->entryFunc = VM_WasmInstantiate( mod.data, mod.len, (void *)VM_WasmSysCall,
		(void *)VM_WasmBlockCopy, (void *)VM_WasmError );
	if ( !w->entryFunc ) {
		Com_Printf( S_COLOR_YELLOW "vm_wasm: %s: instantiation failed, interpreting\n", vm->name );
		WB_Free( &mod );
		goto fail;
	}

	Com_Printf( "vm_wasm: %s: %d functions, %d instructions -> %d KB wasm in %d ms\n",
		vm->name, numFuncs, c.count, mod.len / 1024, Sys_Milliseconds() - t0 );
	WB_Free( &mod );

	Z_Free( c.op ); Z_Free( c.arg ); Z_Free( c.depth ); Z_Free( c.leader );
	vm->codeBase = (byte *)w;
	vm->destroy = VM_Destroy_Wasm;
	vm->compiled = qtrue;
	return;

fail:
	if ( c.op ) { Z_Free( c.op ); Z_Free( c.arg ); Z_Free( c.depth ); Z_Free( c.leader ); }
	if ( w->funcOf ) Z_Free( w->funcOf );
	if ( w->blockOf ) Z_Free( w->blockOf );
	Z_Free( w );
	vm->compiled = qfalse;
}

int VM_CallCompiled( vm_t *vm, int *args ) {
	vmWasm_t *w = (vmWasm_t *)vm->codeBase;
	byte *image = vm->dataBase;
	int stackOnEntry = vm->programStack;
	int programStack = stackOnEntry - ( 8 + 4 * MAX_VMMAIN_ARGS );
	int arg, r;

	// same initial frame as VM_CallInterpreted
	for ( arg = 0; arg < MAX_VMMAIN_ARGS; arg++ )
		*(int *)&image[ programStack + 8 + arg * 4 ] = args[ arg ];
	*(int *)&image[ programStack + 4 ] = 0;
	*(int *)&image[ programStack ] = -1;

	r = ( (int (*)( int ))(intptr_t)w->entryFunc )( programStack );

	vm->programStack = stackOnEntry;
	return r;
}

#endif
