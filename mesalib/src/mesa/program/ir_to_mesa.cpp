/*
 * Copyright (C) 2005-2007  Brian Paul   All Rights Reserved.
 * Copyright (C) 2008  VMware, Inc.   All Rights Reserved.
 * Copyright © 2010 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * \file ir_to_mesa.cpp
 *
 * Translate GLSL IR to Mesa's gl_program representation.
 */

#include <stdio.h>
#include "main/macros.h"
#include "main/mtypes.h"
#include "main/shaderapi.h"
#include "main/shaderobj.h"
#include "main/uniforms.h"
#include "main/glspirv.h"
#include "compiler/glsl/ast.h"
#include "compiler/glsl/ir.h"
#include "compiler/glsl/ir_expression_flattening.h"
#include "compiler/glsl/ir_visitor.h"
#include "compiler/glsl/ir_optimization.h"
#include "compiler/glsl/ir_uniform.h"
#include "compiler/glsl/glsl_parser_extras.h"
#include "compiler/glsl_types.h"
#include "compiler/glsl/linker.h"
#include "compiler/glsl/program.h"
#include "compiler/glsl/shader_cache.h"
#include "compiler/glsl/string_to_uint_map.h"
#include "program/prog_instruction.h"
#include "program/prog_optimize.h"
#include "program/prog_print.h"
#include "program/program.h"
#include "program/prog_parameter.h"


static int swizzle_for_size(int size);

namespace {

class src_reg;
class dst_reg;

/**
 * This struct is a corresponding struct to Mesa prog_src_register, with
 * wider fields.
 */
class src_reg {
public:
   src_reg(gl_register_file file, int index, const glsl_type *type)
   {
      this->file = file;
      this->index = index;
      if (type && (type->is_scalar() || type->is_vector() || type->is_matrix()))
	 this->swizzle = swizzle_for_size(type->vector_elements);
      else
	 this->swizzle = SWIZZLE_XYZW;
      this->negate = 0;
      this->reladdr = NULL;
   }

   src_reg()
   {
      this->file = PROGRAM_UNDEFINED;
      this->index = 0;
      this->swizzle = 0;
      this->negate = 0;
      this->reladdr = NULL;
   }

   explicit src_reg(dst_reg reg);

   gl_register_file file; /**< PROGRAM_* from Mesa */
   int index; /**< temporary index, VERT_ATTRIB_*, VARYING_SLOT_*, etc. */
   GLuint swizzle; /**< SWIZZLE_XYZWONEZERO swizzles from Mesa. */
   int negate; /**< NEGATE_XYZW mask from mesa */
   /** Register index should be offset by the integer in this reg. */
   src_reg *reladdr;
};

class dst_reg {
public:
   dst_reg(gl_register_file file, int writemask)
   {
      this->file = file;
      this->index = 0;
      this->writemask = writemask;
      this->reladdr = NULL;
   }

   dst_reg()
   {
      this->file = PROGRAM_UNDEFINED;
      this->index = 0;
      this->writemask = 0;
      this->reladdr = NULL;
   }

   explicit dst_reg(src_reg reg);

   gl_register_file file; /**< PROGRAM_* from Mesa */
   int index; /**< temporary index, VERT_ATTRIB_*, VARYING_SLOT_*, etc. */
   int writemask; /**< Bitfield of WRITEMASK_[XYZW] */
   /** Register index should be offset by the integer in this reg. */
   src_reg *reladdr;
};

} /* anonymous namespace */

src_reg::src_reg(dst_reg reg)
{
   this->file = reg.file;
   this->index = reg.index;
   this->swizzle = SWIZZLE_XYZW;
   this->negate = 0;
   this->reladdr = reg.reladdr;
}

dst_reg::dst_reg(src_reg reg)
{
   this->file = reg.file;
   this->index = reg.index;
   this->writemask = WRITEMASK_XYZW;
   this->reladdr = reg.reladdr;
}

namespace {

class ir_to_mesa_instruction : public exec_node {
public:
   DECLARE_RALLOC_CXX_OPERATORS(ir_to_mesa_instruction)

   enum prog_opcode op;
   dst_reg dst;
   src_reg src[3];
   /** Pointer to the ir source this tree came from for debugging */
   ir_instruction *ir;
   bool saturate;
   int sampler; /**< sampler index */
   int tex_target; /**< One of TEXTURE_*_INDEX */
   GLboolean tex_shadow;
};

class variable_storage : public exec_node {
public:
   variable_storage(ir_variable *var, gl_register_file file, int index)
      : file(file), index(index), var(var)
   {
      /* empty */
   }

   gl_register_file file;
   int index;
   ir_variable *var; /* variable that maps to this, if any */
};

class function_entry : public exec_node {
public:
   ir_function_signature *sig;

   /**
    * identifier of this function signature used by the program.
    *
    * At the point that Mesa instructions for function calls are
    * generated, we don't know the address of the first instruction of
    * the function body.  So we make the BranchTarget that is called a
    * small integer and rewrite them during set_branchtargets().
    */
   int sig_id;

   /**
    * Pointer to first instruction of the function body.
    *
    * Set during function body emits after main() is processed.
    */
   ir_to_mesa_instruction *bgn_inst;

   /**
    * Index of the first instruction of the function body in actual
    * Mesa IR.
    *
    * Set after convertion from ir_to_mesa_instruction to prog_instruction.
    */
   int inst;

   /** Storage for the return value. */
   src_reg return_reg;
};

class ir_to_mesa_visitor : public ir_visitor {
public:
   ir_to_mesa_visitor();
   ~ir_to_mesa_visitor();

   function_entry *current_function;

   struct gl_context *ctx;
   struct gl_program *prog;
   struct gl_shader_program *shader_program;
   struct gl_shader_compiler_options *options;

   int next_temp;

   variable_storage *find_variable_storage(const ir_variable *var);

   src_reg get_temp(const glsl_type *type);
   void reladdr_to_temp(ir_instruction *ir, src_reg *reg, int *num_reladdr);

   src_reg src_reg_for_float(float val);

   /**
    * \name Visit methods
    *
    * As typical for the visitor pattern, there must be one \c visit method for
    * each concrete subclass of \c ir_instruction.  Virtual base classes within
    * the hierarchy should not have \c visit methods.
    */
   /*@{*/
   virtual void visit(ir_variable *);
   virtual void visit(ir_loop *);
   virtual void visit(ir_loop_jump *);
   virtual void visit(ir_function_signature *);
   virtual void visit(ir_function *);
   virtual void visit(ir_expression *);
   virtual void visit(ir_swizzle *);
   virtual void visit(ir_dereference_variable  *);
   virtual void visit(ir_dereference_array *);
   virtual void visit(ir_dereference_record *);
   virtual void visit(ir_assignment *);
   virtual void visit(ir_constant *);
   virtual void visit(ir_call *);
   virtual void visit(ir_return *);
   virtual void visit(ir_discard *);
   virtual void visit(ir_demote *);
   virtual void visit(ir_texture *);
   virtual void visit(ir_if *);
   virtual void visit(ir_emit_vertex *);
   virtual void visit(ir_end_primitive *);
   virtual void visit(ir_barrier *);
   /*@}*/

   src_reg result;

   /** List of variable_storage */
   exec_list variables;

   /** List of function_entry */
   exec_list function_signatures;
   int next_signature_id;

   /** List of ir_to_mesa_instruction */
   exec_list instructions;

   ir_to_mesa_instruction *emit(ir_instruction *ir, enum prog_opcode op);

   ir_to_mesa_instruction *emit(ir_instruction *ir, enum prog_opcode op,
			        dst_reg dst, src_reg src0);

   ir_to_mesa_instruction *emit(ir_instruction *ir, enum prog_opcode op,
			        dst_reg dst, src_reg src0, src_reg src1);

   ir_to_mesa_instruction *emit(ir_instruction *ir, enum prog_opcode op,
			        dst_reg dst,
			        src_reg src0, src_reg src1, src_reg src2);

   /**
    * Emit the correct dot-product instruction for the type of arguments
    */
   ir_to_mesa_instruction * emit_dp(ir_instruction *ir,
				    dst_reg dst,
				    src_reg src0,
				    src_reg src1,
				    unsigned elements);

   void emit_scalar(ir_instruction *ir, enum prog_opcode op,
		    dst_reg dst, src_reg src0);

   void emit_scalar(ir_instruction *ir, enum prog_opcode op,
		    dst_reg dst, src_reg src0, src_reg src1);

   bool try_emit_mad(ir_expression *ir,
			  int mul_operand);
   bool try_emit_mad_for_and_not(ir_expression *ir,
				 int mul_operand);

   void emit_swz(ir_expression *ir);

   void emit_equality_comparison(ir_expression *ir, enum prog_opcode op,
                                 dst_reg dst,
                                 const src_reg &src0, const src_reg &src1);

   inline void emit_sne(ir_expression *ir, dst_reg dst,
                        const src_reg &src0, const src_reg &src1)
   {
      emit_equality_comparison(ir, OPCODE_SLT, dst, src0, src1);
   }

   inline void emit_seq(ir_expression *ir, dst_reg dst,
                        const src_reg &src0, const src_reg &src1)
   {
      emit_equality_comparison(ir, OPCODE_SGE, dst, src0, src1);
   }

   bool process_move_condition(ir_rvalue *ir);

   void copy_propagate(void);

   void *mem_ctx;
};

} /* anonymous namespace */

static src_reg undef_src = src_reg(PROGRAM_UNDEFINED, 0, NULL);

static dst_reg undef_dst = dst_reg(PROGRAM_UNDEFINED, SWIZZLE_NOOP);

static dst_reg address_reg = dst_reg(PROGRAM_ADDRESS, WRITEMASK_X);

static int
swizzle_for_size(int size)
{
   static const int size_swizzles[4] = {
      MAKE_SWIZZLE4(SWIZZLE_X, SWIZZLE_X, SWIZZLE_X, SWIZZLE_X),
      MAKE_SWIZZLE4(SWIZZLE_X, SWIZZLE_Y, SWIZZLE_Y, SWIZZLE_Y),
      MAKE_SWIZZLE4(SWIZZLE_X, SWIZZLE_Y, SWIZZLE_Z, SWIZZLE_Z),
      MAKE_SWIZZLE4(SWIZZLE_X, SWIZZLE_Y, SWIZZLE_Z, SWIZZLE_W),
   };

   assert((size >= 1) && (size <= 4));
   return size_swizzles[size - 1];
}

ir_to_mesa_instruction *
ir_to_mesa_visitor::emit(ir_instruction *ir, enum prog_opcode op,
			 dst_reg dst,
			 src_reg src0, src_reg src1, src_reg src2)
{
   ir_to_mesa_instruction *inst = new(mem_ctx) ir_to_mesa_instruction();
   int num_reladdr = 0;

   /* If we have to do relative addressing, we want to load the ARL
    * reg directly for one of the regs, and preload the other reladdr
    * sources into temps.
    */
   num_reladdr += dst.reladdr != NULL;
   num_reladdr += src0.reladdr != NULL;
   num_reladdr += src1.reladdr != NULL;
   num_reladdr += src2.reladdr != NULL;

   reladdr_to_temp(ir, &src2, &num_reladdr);
   reladdr_to_temp(ir, &src1, &num_reladdr);
   reladdr_to_temp(ir, &src0, &num_reladdr);

   if (dst.reladdr) {
      emit(ir, OPCODE_ARL, address_reg, *dst.reladdr);
      num_reladdr--;
   }
   assert(num_reladdr == 0);

   inst->op = op;
   inst->dst = dst;
   inst->src[0] = src0;
   inst->src[1] = src1;
   inst->src[2] = src2;
   inst->ir = ir;

   this->instructions.push_tail(inst);

   return inst;
}


ir_to_mesa_instruction *
ir_to_mesa_visitor::emit(ir_instruction *ir, enum prog_opcode op,
			 dst_reg dst, src_reg src0, src_reg src1)
{
   return emit(ir, op, dst, src0, src1, undef_src);
}

ir_to_mesa_instruction *
ir_to_mesa_visitor::emit(ir_instruction *ir, enum prog_opcode op,
			 dst_reg dst, src_reg src0)
{
   assert(dst.writemask != 0);
   return emit(ir, op, dst, src0, undef_src, undef_src);
}

ir_to_mesa_instruction *
ir_to_mesa_visitor::emit(ir_instruction *ir, enum prog_opcode op)
{
   return emit(ir, op, undef_dst, undef_src, undef_src, undef_src);
}

ir_to_mesa_instruction *
ir_to_mesa_visitor::emit_dp(ir_instruction *ir,
			    dst_reg dst, src_reg src0, src_reg src1,
			    unsigned elements)
{
   static const enum prog_opcode dot_opcodes[] = {
      OPCODE_DP2, OPCODE_DP3, OPCODE_DP4
   };

   return emit(ir, dot_opcodes[elements - 2], dst, src0, src1);
}

/**
 * Emits Mesa scalar opcodes to produce unique answers across channels.
 *
 * Some Mesa opcodes are scalar-only, like ARB_fp/vp.  The src X
 * channel determines the result across all channels.  So to do a vec4
 * of this operation, we want to emit a scalar per source channel used
 * to produce dest channels.
 */
void
ir_to_mesa_visitor::emit_scalar(ir_instruction *ir, enum prog_opcode op,
			        dst_reg dst,
				src_reg orig_src0, src_reg orig_src1)
{
   int i, j;
   int done_mask = ~dst.writemask;

   /* Mesa RCP is a scalar operation splatting results to all channels,
    * like ARB_fp/vp.  So emit as many RCPs as necessary to cover our
    * dst channels.
    */
   for (i = 0; i < 4; i++) {
      GLuint this_mask = (1 << i);
      ir_to_mesa_instruction *inst;
      src_reg src0 = orig_src0;
      src_reg src1 = orig_src1;

      if (done_mask & this_mask)
	 continue;

      GLuint src0_swiz = GET_SWZ(src0.swizzle, i);
      GLuint src1_swiz = GET_SWZ(src1.swizzle, i);
      for (j = i + 1; j < 4; j++) {
	 /* If there is another enabled component in the destination that is
	  * derived from the same inputs, generate its value on this pass as
	  * well.
	  */
	 if (!(done_mask & (1 << j)) &&
	     GET_SWZ(src0.swizzle, j) == src0_swiz &&
	     GET_SWZ(src1.swizzle, j) == src1_swiz) {
	    this_mask |= (1 << j);
	 }
      }
      src0.swizzle = MAKE_SWIZZLE4(src0_swiz, src0_swiz,
				   src0_swiz, src0_swiz);
      src1.swizzle = MAKE_SWIZZLE4(src1_swiz, src1_swiz,
				  src1_swiz, src1_swiz);

      inst = emit(ir, op, dst, src0, src1);
      inst->dst.writemask = this_mask;
      done_mask |= this_mask;
   }
}

void
ir_to_mesa_visitor::emit_scalar(ir_instruction *ir, enum prog_opcode op,
			        dst_reg dst, src_reg src0)
{
   src_reg undef = undef_src;

   undef.swizzle = SWIZZLE_XXXX;

   emit_scalar(ir, op, dst, src0, undef);
}

src_reg
ir_to_mesa_visitor::src_reg_for_float(float val)
{
   src_reg src(PROGRAM_CONSTANT, -1, NULL);

   src.index = _mesa_add_unnamed_constant(this->prog->Parameters,
					  (const gl_constant_value *)&val, 1, &src.swizzle);

   return src;
}

static int
storage_type_size(const struct glsl_type *type, bool bindless)
{
   unsigned int i;
   int size;

   switch (type->base_type) {
   case GLSL_TYPE_UINT:
   case GLSL_TYPE_INT:
   case GLSL_TYPE_UINT8:
   case GLSL_TYPE_INT8:
   case GLSL_TYPE_UINT16:
   case GLSL_TYPE_INT16:
   case GLSL_TYPE_FLOAT:
   case GLSL_TYPE_FLOAT16:
   case GLSL_TYPE_BOOL:
      if (type->is_matrix()) {
	 return type->matrix_columns;
      } else {
	 /* Regardless of size of vector, it gets a vec4. This is bad
	  * packing for things like floats, but otherwise arrays become a
	  * mess.  Hopefully a later pass over the code can pack scalars
	  * down if appropriate.
	  */
	 return 1;
      }
      break;
   case GLSL_TYPE_DOUBLE:
      if (type->is_matrix()) {
         if (type->vector_elements > 2)
            return type->matrix_columns * 2;
         else
            return type->matrix_columns;
      } else {
         if (type->vector_elements > 2)
            return 2;
         else
            return 1;
      }
      break;
   case GLSL_TYPE_UINT64:
   case GLSL_TYPE_INT64:
      if (type->vector_elements > 2)
         return 2;
      else
         return 1;
   case GLSL_TYPE_ARRAY:
      assert(type->length > 0);
      return storage_type_size(type->fields.array, bindless) * type->length;
   case GLSL_TYPE_STRUCT:
      size = 0;
      for (i = 0; i < type->length; i++) {
	 size += storage_type_size(type->fields.structure[i].type, bindless);
      }
      return size;
   case GLSL_TYPE_SAMPLER:
   case GLSL_TYPE_IMAGE:
      if (!bindless)
         return 0;
      /* fall through */
   case GLSL_TYPE_SUBROUTINE:
      return 1;
   case GLSL_TYPE_ATOMIC_UINT:
   case GLSL_TYPE_VOID:
   case GLSL_TYPE_ERROR:
   case GLSL_TYPE_INTERFACE:
   case GLSL_TYPE_FUNCTION:
      assert(!"Invalid type in type_size");
      break;
   }

   return 0;
}

static int
type_size(const struct glsl_type *type)
{
   return storage_type_size(type, false);
}

/**
 * In the initial pass of codegen, we assign temporary numbers to
 * intermediate results.  (not SSA -- variable assignments will reuse
 * storage).  Actual register allocation for the Mesa VM occurs in a
 * pass over the Mesa IR later.
 */
src_reg
ir_to_mesa_visitor::get_temp(const glsl_type *type)
{
   src_reg src;

   src.file = PROGRAM_TEMPORARY;
   src.index = next_temp;
   src.reladdr = NULL;
   next_temp += type_size(type);

   if (type->is_array() || type->is_struct()) {
      src.swizzle = SWIZZLE_NOOP;
   } else {
      src.swizzle = swizzle_for_size(type->vector_elements);
   }
   src.negate = 0;

   return src;
}

variable_storage *
ir_to_mesa_visitor::find_variable_storage(const ir_variable *var)
{
   foreach_in_list(variable_storage, entry, &this->variables) {
      if (entry->var == var)
	 return entry;
   }

   return NULL;
}

void
ir_to_mesa_visitor::visit(ir_variable *ir)
{
   if (ir->data.mode == ir_var_uniform && strncmp(ir->name, "gl_", 3) == 0) {
      unsigned int i;
      const ir_state_slot *const slots = ir->get_state_slots();
      assert(slots != NULL);

      /* Check if this statevar's setup in the STATE file exactly
       * matches how we'll want to reference it as a
       * struct/array/whatever.  If not, then we need to move it into
       * temporary storage and hope that it'll get copy-propagated
       * out.
       */
      for (i = 0; i < ir->get_num_state_slots(); i++) {
	 if (slots[i].swizzle != SWIZZLE_XYZW) {
	    break;
	 }
      }

      variable_storage *storage;
      dst_reg dst;
      if (i == ir->get_num_state_slots()) {
	 /* We'll set the index later. */
	 storage = new(mem_ctx) variable_storage(ir, PROGRAM_STATE_VAR, -1);
	 this->variables.push_tail(storage);

	 dst = undef_dst;
      } else {
	 /* The variable_storage constructor allocates slots based on the size
	  * of the type.  However, this had better match the number of state
	  * elements that we're going to copy into the new temporary.
	  */
	 assert((int) ir->get_num_state_slots() == type_size(ir->type));

	 storage = new(mem_ctx) variable_storage(ir, PROGRAM_TEMPORARY,
						 this->next_temp);
	 this->variables.push_tail(storage);
	 this->next_temp += type_size(ir->type);

	 dst = dst_reg(src_reg(PROGRAM_TEMPORARY, storage->index, NULL));
      }


      for (unsigned int i = 0; i < ir->get_num_state_slots(); i++) {
	 int index = _mesa_add_state_reference(this->prog->Parameters,
					       slots[i].tokens);

	 if (storage->file == PROGRAM_STATE_VAR) {
	    if (storage->index == -1) {
	       storage->index = index;
	    } else {
	       assert(index == storage->index + (int)i);
	    }
	 } else {
	    src_reg src(PROGRAM_STATE_VAR, index, NULL);
	    src.swizzle = slots[i].swizzle;
	    emit(ir, OPCODE_MOV, dst, src);
	    /* even a float takes up a whole vec4 reg in a struct/array. */
	    dst.index++;
	 }
      }

      if (storage->file == PROGRAM_TEMPORARY &&
	  dst.index != storage->index + (int) ir->get_num_state_slots()) {
	 linker_error(this->shader_program,
		      "failed to load builtin uniform `%s' "
		      "(%d/%d regs loaded)\n",
		      ir->name, dst.index - storage->index,
		      type_size(ir->type));
      }
   }
}

void
ir_to_mesa_visitor::visit(ir_loop *ir)
{
   emit(NULL, OPCODE_BGNLOOP);

   visit_exec_list(&ir->body_instructions, this);

   emit(NULL, OPCODE_ENDLOOP);
}

void
ir_to_mesa_visitor::visit(ir_loop_jump *ir)
{
   switch (ir->mode) {
   case ir_loop_jump::jump_break:
      emit(NULL, OPCODE_BRK);
      break;
   case ir_loop_jump::jump_continue:
      emit(NULL, OPCODE_CONT);
      break;
   }
}


void
ir_to_mesa_visitor::visit(ir_function_signature *ir)
{
   assert(0);
   (void)ir;
}

void
ir_to_mesa_visitor::visit(ir_function *ir)
{
   /* Ignore function bodies other than main() -- we shouldn't see calls to
    * them since they should all be inlined before we get to ir_to_mesa.
    */
   if (strcmp(ir->name, "main") == 0) {
      const ir_function_signature *sig;
      exec_list empty;

      sig = ir->matching_signature(NULL, &empty, false);

      assert(sig);

      foreach_in_list(ir_instruction, ir, &sig->body) {
	 ir->accept(this);
      }
   }
}

bool
ir_to_mesa_visitor::try_emit_mad(ir_expression *ir, int mul_operand)
{
   int nonmul_operand = 1 - mul_operand;
   src_reg a, b, c;

   ir_expression *expr = ir->operands[mul_operand]->as_expression();
   if (!expr || expr->operation != ir_binop_mul)
      return false;

   expr->operands[0]->accept(this);
   a = this->result;
   expr->operands[1]->accept(this);
   b = this->result;
   ir->operands[nonmul_operand]->accept(this);
   c = this->result;

   this->result = get_temp(ir->type);
   emit(ir, OPCODE_MAD, dst_reg(this->result), a, b, c);

   return true;
}

/**
 * Emit OPCODE_MAD(a, -b, a) instead of AND(a, NOT(b))
 *
 * The logic values are 1.0 for true and 0.0 for false.  Logical-and is
 * implemented using multiplication, and logical-or is implemented using
 * addition.  Logical-not can be implemented as (true - x), or (1.0 - x).
 * As result, the logical expression (a & !b) can be rewritten as:
 *
 *     - a * !b
 *     - a * (1 - b)
 *     - (a * 1) - (a * b)
 *     - a + -(a * b)
 *     - a + (a * -b)
 *
 * This final expression can be implemented as a single MAD(a, -b, a)
 * instruction.
 */
bool
ir_to_mesa_visitor::try_emit_mad_for_and_not(ir_expression *ir, int try_operand)
{
   const int other_operand = 1 - try_operand;
   src_reg a, b;

   ir_expression *expr = ir->operands[try_operand]->as_expression();
   if (!expr || expr->operation != ir_unop_logic_not)
      return false;

   ir->operands[other_operand]->accept(this);
   a = this->result;
   expr->operands[0]->accept(this);
   b = this->result;

   b.negate = ~b.negate;

   this->result = get_temp(ir->type);
   emit(ir, OPCODE_MAD, dst_reg(this->result), a, b, a);

   return true;
}

void
ir_to_mesa_visitor::reladdr_to_temp(ir_instruction *ir,
				    src_reg *reg, int *num_reladdr)
{
   if (!reg->reladdr)
      return;

   emit(ir, OPCODE_ARL, address_reg, *reg->reladdr);

   if (*num_reladdr != 1) {
      src_reg temp = get_temp(glsl_type::vec4_type);

      emit(ir, OPCODE_MOV, dst_reg(temp), *reg);
      *reg = temp;
   }

   (*num_reladdr)--;
}

void
ir_to_mesa_visitor::emit_swz(ir_expression *ir)
{
   /* Assume that the vector operator is in a form compatible with OPCODE_SWZ.
    * This means that each of the operands is either an immediate value of -1,
    * 0, or 1, or is a component from one source register (possibly with
    * negation).
    */
   uint8_t components[4] = { 0 };
   bool negate[4] = { false };
   ir_variable *var = NULL;

   for (unsigned i = 0; i < ir->type->vector_elements; i++) {
      ir_rvalue *op = ir->operands[i];

      assert(op->type->is_scalar());

      while (op != NULL) {
	 switch (op->ir_type) {
	 case ir_type_constant: {

	    assert(op->type->is_scalar());

	    const ir_constant *const c = op->as_constant();
	    if (c->is_one()) {
	       components[i] = SWIZZLE_ONE;
	    } else if (c->is_zero()) {
	       components[i] = SWIZZLE_ZERO;
	    } else if (c->is_negative_one()) {
	       components[i] = SWIZZLE_ONE;
	       negate[i] = true;
	    } else {
	       assert(!"SWZ constant must be 0.0 or 1.0.");
	    }

	    op = NULL;
	    break;
	 }

	 case ir_type_dereference_variable: {
	    ir_dereference_variable *const deref =
	       (ir_dereference_variable *) op;

	    assert((var == NULL) || (deref->var == var));
	    components[i] = SWIZZLE_X;
	    var = deref->var;
	    op = NULL;
	    break;
	 }

	 case ir_type_expression: {
	    ir_expression *const expr = (ir_expression *) op;

	    assert(expr->operation == ir_unop_neg);
	    negate[i] = true;

	    op = expr->operands[0];
	    break;
	 }

	 case ir_type_swizzle: {
	    ir_swizzle *const swiz = (ir_swizzle *) op;

	    components[i] = swiz->mask.x;
	    op = swiz->val;
	    break;
	 }

	 default:
	    assert(!"Should not get here.");
	    return;
	 }
      }
   }

   assert(var != NULL);

   ir_dereference_variable *const deref =
      new(mem_ctx) ir_dereference_variable(var);

   this->result.file = PROGRAM_UNDEFINED;
   deref->accept(this);
   if (this->result.file == PROGRAM_UNDEFINED) {
      printf("Failed to get tree for expression operand:\n");
      deref->print();
      printf("\n");
      exit(1);
   }

   src_reg src;

   src = this->result;
   src.swizzle = MAKE_SWIZZLE4(components[0],
			       components[1],
			       components[2],
			       components[3]);
   src.negate = ((unsigned(negate[0]) << 0)
		 | (unsigned(negate[1]) << 1)
		 | (unsigned(negate[2]) << 2)
		 | (unsigned(negate[3]) << 3));

   /* Storage for our result.  Ideally for an assignment we'd be using the
    * actual storage for the result here, instead.
    */
   const src_reg result_src = get_temp(ir->type);
   dst_reg result_dst = dst_reg(result_src);

   /* Limit writes to the channels that will be used by result_src later.
    * This does limit this temp's use as a temporary for multi-instruction
    * sequences.
    */
   result_dst.writemask = (1 << ir->type->vector_elements) - 1;

   emit(ir, OPCODE_SWZ, result_dst, src);
   this->result = result_src;
}

void
ir_to_mesa_visitor::emit_equality_comparison(ir_expression *ir,
                                             enum prog_opcode op,
                                             dst_reg dst,
                                             const src_reg &src0,
                                             const src_reg &src1)
{
   src_reg difference;
   src_reg abs_difference = get_temp(glsl_type::vec4_type);
   const src_reg zero = src_reg_for_float(0.0);

   /* x == y is equivalent to -abs(x-y) >= 0.  Since all of the code that
    * consumes the generated IR is pretty dumb, take special care when one
    * of the operands is zero.
    *
    * Similarly, x != y is equivalent to -abs(x-y) < 0.
    */
   if (src0.file == zero.file &&
       src0.index == zero.index &&
       src0.swizzle == zero.swizzle) {
      difference = src1;
   } else if (src1.file == zero.file &&
              src1.index == zero.index &&
              src1.swizzle == zero.swizzle) {
      difference = src0;
   } else {
      difference = get_temp(glsl_type::vec4_type);

      src_reg tmp_src = src0;
      tmp_src.negate = ~tmp_src.negate;

      emit(ir, OPCODE_ADD, dst_reg(difference), tmp_src, src1);
   }

   emit(ir, OPCODE_ABS, dst_reg(abs_difference), difference);

   abs_difference.negate = ~abs_difference.negate;
   emit(ir, op, dst, abs_difference, zero);
}

void
ir_to_mesa_visitor::visit(ir_expression *ir)
{
   unsigned int operand;
   src_reg op[ARRAY_SIZE(ir->operands)];
   src_reg result_src;
   dst_reg result_dst;

   /* Quick peephole: Emit OPCODE_MAD(a, b, c) instead of ADD(MUL(a, b), c)
    */
   if (ir->operation == ir_binop_add) {
      if (try_emit_mad(ir, 1))
	 return;
      if (try_emit_mad(ir, 0))
	 return;
   }

   /* Quick peephole: Emit OPCODE_MAD(-a, -b, a) instead of AND(a, NOT(b))
    */
   if (ir->operation == ir_binop_logic_and) {
      if (try_emit_mad_for_and_not(ir, 1))
	 return;
      if (try_emit_mad_for_and_not(ir, 0))
	 return;
   }

   if (ir->operation == ir_quadop_vector) {
      this->emit_swz(ir);
      return;
   }

   for (operand = 0; operand < ir->num_operands; operand++) {
      this->result.file = PROGRAM_UNDEFINED;
      ir->operands[operand]->accept(this);
      if (this->result.file == PROGRAM_UNDEFINED) {
	 printf("Failed to get tree for expression operand:\n");
         ir->operands[operand]->print();
         printf("\n");
	 exit(1);
      }
      op[operand] = this->result;

      /* Matrix expression operands should have been broken down to vector
       * operations already.
       */
      assert(!ir->operands[operand]->type->is_matrix());
   }

   int vector_elements = ir->operands[0]->type->vector_elements;
   if (ir->operands[1]) {
      vector_elements = MAX2(vector_elements,
			     ir->operands[1]->type->vector_elements);
   }

   this->result.file = PROGRAM_UNDEFINED;

   /* Storage for our result.  Ideally for an assignment we'd be using
    * the actual storage for the result here, instead.
    */
   result_src = get_temp(ir->type);
   /* convenience for the emit functions below. */
   result_dst = dst_reg(result_src);
   /* Limit writes to the channels that will be used by result_src later.
    * This does limit this temp's use as a temporary for multi-instruction
    * sequences.
    */
   result_dst.writemask = (1 << ir->type->vector_elements) - 1;

   switch (ir->operation) {
   case ir_unop_logic_not:
      /* Previously 'SEQ dst, src, 0.0' was used for this.  However, many
       * older GPUs implement SEQ using multiple instructions (i915 uses two
       * SGE instructions and a MUL instruction).  Since our logic values are
       * 0.0 and 1.0, 1-x also implements !x.
       */
      op[0].negate = ~op[0].negate;
      emit(ir, OPCODE_ADD, result_dst, op[0], src_reg_for_float(1.0));
      break;
   case ir_unop_neg:
      op[0].negate = ~op[0].negate;
      result_src = op[0];
      break;
   case ir_unop_abs:
      emit(ir, OPCODE_ABS, result_dst, op[0]);
      break;
   case ir_unop_sign:
      emit(ir, OPCODE_SSG, result_dst, op[0]);
      break;
   case ir_unop_rcp:
      emit_scalar(ir, OPCODE_RCP, result_dst, op[0]);
      break;

   case ir_unop_exp2:
      emit_scalar(ir, OPCODE_EX2, result_dst, op[0]);
      break;
   case ir_unop_exp:
      assert(!"not reached: should be handled by exp_to_exp2");
      break;
   case ir_unop_log:
      assert(!"not reached: should be handled by log_to_log2");
      break;
   case ir_unop_log2:
      emit_scalar(ir, OPCODE_LG2, result_dst, op[0]);
      break;
   case ir_unop_sin:
      emit_scalar(ir, OPCODE_SIN, result_dst, op[0]);
      break;
   case ir_unop_cos:
      emit_scalar(ir, OPCODE_COS, result_dst, op[0]);
      break;

   case ir_unop_dFdx:
      emit(ir, OPCODE_DDX, result_dst, op[0]);
      break;
   case ir_unop_dFdy:
      emit(ir, OPCODE_DDY, result_dst, op[0]);
      break;

   case ir_unop_saturate: {
      ir_to_mesa_instruction *inst = emit(ir, OPCODE_MOV,
                                          result_dst, op[0]);
      inst->saturate = true;
      break;
   }
   case ir_unop_noise: {
      const enum prog_opcode opcode =
	 prog_opcode(OPCODE_NOISE1
		     + (ir->operands[0]->type->vector_elements) - 1);
      assert((opcode >= OPCODE_NOISE1) && (opcode <= OPCODE_NOISE4));

      emit(ir, opcode, result_dst, op[0]);
      break;
   }

   case ir_binop_add:
      emit(ir, OPCODE_ADD, result_dst, op[0], op[1]);
      break;
   case ir_binop_sub:
      emit(ir, OPCODE_SUB, result_dst, op[0], op[1]);
      break;

   case ir_binop_mul:
      emit(ir, OPCODE_MUL, result_dst, op[0], op[1]);
      break;
   case ir_binop_div:
      assert(!"not reached: should be handled by ir_div_to_mul_rcp");
      break;
   case ir_binop_mod:
      /* Floating point should be lowered by MOD_TO_FLOOR in the compiler. */
      assert(ir->type->is_integer_32());
      emit(ir, OPCODE_MUL, result_dst, op[0], op[1]);
      break;

   case ir_binop_less:
      emit(ir, OPCODE_SLT, result_dst, op[0], op[1]);
      break;
   case ir_binop_gequal:
      emit(ir, OPCODE_SGE, result_dst, op[0], op[1]);
      break;
   case ir_binop_equal:
      emit_seq(ir, result_dst, op[0], op[1]);
      break;
   case ir_binop_nequal:
      emit_sne(ir, result_dst, op[0], op[1]);
      break;
   case ir_binop_all_equal:
      /* "==" operator producing a scalar boolean. */
      if (ir->operands[0]->type->is_vector() ||
	  ir->operands[1]->type->is_vector()) {
	 src_reg temp = get_temp(glsl_type::vec4_type);
         emit_sne(ir, dst_reg(temp), op[0], op[1]);

	 /* After the dot-product, the value will be an integer on the
	  * range [0,4].  Zero becomes 1.0, and positive values become zero.
	  */
	 emit_dp(ir, result_dst, temp, temp, vector_elements);

	 /* Negating the result of the dot-product gives values on the range
	  * [-4, 0].  Zero becomes 1.0, and negative values become zero.  This
	  * achieved using SGE.
	  */
	 src_reg sge_src = result_src;
	 sge_src.negate = ~sge_src.negate;
	 emit(ir, OPCODE_SGE, result_dst, sge_src, src_reg_for_float(0.0));
      } else {
         emit_seq(ir, result_dst, op[0], op[1]);
      }
      break;
   case ir_binop_any_nequal:
      /* "!=" operator producing a scalar boolean. */
      if (ir->operands[0]->type->is_vector() ||
	  ir->operands[1]->type->is_vector()) {
	 src_reg temp = get_temp(glsl_type::vec4_type);
         if (ir->operands[0]->type->is_boolean() &&
             ir->operands[1]->as_constant() &&
             ir->operands[1]->as_constant()->is_zero()) {
            temp = op[0];
         } else {
            emit_sne(ir, dst_reg(temp), op[0], op[1]);
         }

	 /* After the dot-product, the value will be an integer on the
	  * range [0,4].  Zero stays zero, and positive values become 1.0.
	  */
	 ir_to_mesa_instruction *const dp =
	    emit_dp(ir, result_dst, temp, temp, vector_elements);
	 if (this->prog->Target == GL_FRAGMENT_PROGRAM_ARB) {
	    /* The clamping to [0,1] can be done for free in the fragment
	     * shader with a saturate.
	     */
	    dp->saturate = true;
	 } else {
	    /* Negating the result of the dot-product gives values on the range
	     * [-4, 0].  Zero stays zero, and negative values become 1.0.  This
	     * achieved using SLT.
	     */
	    src_reg slt_src = result_src;
	    slt_src.negate = ~slt_src.negate;
	    emit(ir, OPCODE_SLT, result_dst, slt_src, src_reg_for_float(0.0));
	 }
      } else {
         emit_sne(ir, result_dst, op[0], op[1]);
      }
      break;

   case ir_binop_logic_xor:
      emit_sne(ir, result_dst, op[0], op[1]);
      break;

   case ir_binop_logic_or: {
      if (this->prog->Target == GL_FRAGMENT_PROGRAM_ARB) {
         /* After the addition, the value will be an integer on the
          * range [0,2].  Zero stays zero, and positive values become 1.0.
          */
         ir_to_mesa_instruction *add =
            emit(ir, OPCODE_ADD, result_dst, op[0], op[1]);
	 add->saturate = true;
      } else {
         /* The Boolean arguments are stored as float 0.0 and 1.0.  If either
          * value is 1.0, the result of the logcal-or should be 1.0.  If both
          * values are 0.0, the result should be 0.0.  This is exactly what
          * MAX does.
          */
         emit(ir, OPCODE_MAX, result_dst, op[0], op[1]);
      }
      break;
   }

   case ir_binop_logic_and:
      /* the bool args are stored as float 0.0 or 1.0, so "mul" gives us "and". */
      emit(ir, OPCODE_MUL, result_dst, op[0], op[1]);
      break;

   case ir_binop_dot:
      assert(ir->operands[0]->type->is_vector());
      assert(ir->operands[0]->type == ir->operands[1]->type);
      emit_dp(ir, result_dst, op[0], op[1],
	      ir->operands[0]->type->vector_elements);
      break;

   case ir_unop_sqrt:
      /* sqrt(x) = x * rsq(x). */
      emit_scalar(ir, OPCODE_RSQ, result_dst, op[0]);
      emit(ir, OPCODE_MUL, result_dst, result_src, op[0]);
      /* For incoming channels <= 0, set the result to 0. */
      op[0].negate = ~op[0].negate;
      emit(ir, OPCODE_CMP, result_dst,
			  op[0], result_src, src_reg_for_float(0.0));
      break;
   case ir_unop_rsq:
      emit_scalar(ir, OPCODE_RSQ, result_dst, op[0]);
      break;
   case ir_unop_i2f:
   case ir_unop_u2f:
   case ir_unop_b2f:
   case ir_unop_b2i:
   case ir_unop_i2u:
   case ir_unop_u2i:
      /* Mesa IR lacks types, ints are stored as truncated floats. */
      result_src = op[0];
      break;
   case ir_unop_f2i:
   case ir_unop_f2u:
      emit(ir, OPCODE_TRUNC, result_dst, op[0]);
      break;
   case ir_unop_f2b:
   case ir_unop_i2b:
      emit_sne(ir, result_dst, op[0], src_reg_for_float(0.0));
      break;
   case ir_unop_bitcast_f2i: // Ignore these 4, they can't happen here anyway
   case ir_unop_bitcast_f2u:
   case ir_unop_bitcast_i2f:
   case ir_unop_bitcast_u2f:
      break;
   case ir_unop_trunc:
      emit(ir, OPCODE_TRUNC, result_dst, op[0]);
      break;
   case ir_unop_ceil:
      op[0].negate = ~op[0].negate;
      emit(ir, OPCODE_FLR, result_dst, op[0]);
      result_src.negate = ~result_src.negate;
      break;
   case ir_unop_floor:
      emit(ir, OPCODE_FLR, result_dst, op[0]);
      break;
   case ir_unop_fract:
      emit(ir, OPCODE_FRC, result_dst, op[0]);
      break;
   case ir_unop_pack_snorm_2x16:
   case ir_unop_pack_snorm_4x8:
   case ir_unop_pack_unorm_2x16:
   case ir_unop_pack_unorm_4x8:
   case ir_unop_pack_half_2x16:
   case ir_unop_pack_double_2x32:
   case ir_unop_unpack_snorm_2x16:
   case ir_unop_unpack_snorm_4x8:
   case ir_unop_unpack_unorm_2x16:
   case ir_unop_unpack_unorm_4x8:
   case ir_unop_unpack_half_2x16:
   case ir_unop_unpack_double_2x32:
   case ir_unop_bitfield_reverse:
   case ir_unop_bit_count:
   case ir_unop_find_msb:
   case ir_unop_find_lsb:
   case ir_unop_d2f:
   case ir_unop_f2d:
   case ir_unop_d2i:
   case ir_unop_i2d:
   case ir_unop_d2u:
   case ir_unop_u2d:
   case ir_unop_d2b:
   case ir_unop_frexp_sig:
   case ir_unop_frexp_exp:
      assert(!"not supported");
      break;
   case ir_binop_min:
      emit(ir, OPCODE_MIN, result_dst, op[0], op[1]);
      break;
   case ir_binop_max:
      emit(ir, OPCODE_MAX, result_dst, op[0], op[1]);
      break;
   case ir_binop_pow:
      emit_scalar(ir, OPCODE_POW, result_dst, op[0], op[1]);
      break;

      /* GLSL 1.30 integer ops are unsupported in Mesa IR, but since
       * hardware backends have no way to avoid Mesa IR generation
       * even if they don't use it, we need to emit "something" and
       * continue.
       */
   case ir_binop_lshift:
   case ir_binop_rshift:
   case ir_binop_bit_and:
   case ir_binop_bit_xor:
   case ir_binop_bit_or:
      emit(ir, OPCODE_ADD, result_dst, op[0], op[1]);
      break;

   case ir_unop_bit_not:
   case ir_unop_round_even:
      emit(ir, OPCODE_MOV, result_dst, op[0]);
      break;

   case ir_binop_ubo_load:
      assert(!"not supported");
      break;

   case ir_triop_lrp:
      /* ir_triop_lrp operands are (x, y, a) while
       * OPCODE_LRP operands are (a, y, x) to match ARB_fragment_program.
       */
      emit(ir, OPCODE_LRP, result_dst, op[2], op[1], op[0]);
      break;

   case ir_triop_csel:
      /* We assume that boolean true and false are 1.0 and 0.0.  OPCODE_CMP
       * selects src1 if src0 is < 0, src2 otherwise.
       */
      op[0].negate = ~op[0].negate;
      emit(ir, OPCODE_CMP, result_dst, op[0], op[1], op[2]);
      break;

   case ir_binop_vector_extract:
   case ir_triop_fma:
   case ir_triop_bitfield_extract:
   case ir_triop_vector_insert:
   case ir_quadop_bitfield_insert:
   case ir_binop_ldexp:
   case ir_binop_carry:
   case ir_binop_borrow:
   case ir_binop_imul_high:
   case ir_unop_interpolate_at_centroid:
   case ir_binop_interpolate_at_offset:
   case ir_binop_interpolate_at_sample:
   case ir_unop_dFdx_coarse:
   case ir_unop_dFdx_fine:
   case ir_unop_dFdy_coarse:
   case ir_unop_dFdy_fine:
   case ir_unop_subroutine_to_int:
   case ir_unop_get_buffer_size:
   case ir_unop_bitcast_u642d:
   case ir_unop_bitcast_i642d:
   case ir_unop_bitcast_d2u64:
   case ir_unop_bitcast_d2i64:
   case ir_unop_i642i:
   case ir_unop_u642i:
   case ir_unop_i642u:
   case ir_unop_u642u:
   case ir_unop_i642b:
   case ir_unop_i642f:
   case ir_unop_u642f:
   case ir_unop_i642d:
   case ir_unop_u642d:
   case ir_unop_i2i64:
   case ir_unop_u2i64:
   case ir_unop_b2i64:
   case ir_unop_f2i64:
   case ir_unop_d2i64:
   case ir_unop_i2u64:
   case ir_unop_u2u64:
   case ir_unop_f2u64:
   case ir_unop_d2u64:
   case ir_unop_u642i64:
   case ir_unop_i642u64:
   case ir_unop_pack_int_2x32:
   case ir_unop_unpack_int_2x32:
   case ir_unop_pack_uint_2x32:
   case ir_unop_unpack_uint_2x32:
   case ir_unop_pack_sampler_2x32:
   case ir_unop_unpack_sampler_2x32:
   case ir_unop_pack_image_2x32:
   case ir_unop_unpack_image_2x32:
   case ir_unop_atan:
   case ir_binop_atan2:
      assert(!"not supported");
      break;

   case ir_unop_ssbo_unsized_array_length:
   case ir_quadop_vector:
      /* This operation should have already been handled.
       */
      assert(!"Should not get here.");
      break;
   }

   this->result = result_src;
}


void
ir_to_mesa_visitor::visit(ir_swizzle *ir)
{
   src_reg src;
   int i;
   int swizzle[4];

   /* Note that this is only swizzles in expressions, not those on the left
    * hand side of an assignment, which do write masking.  See ir_assignment
    * for that.
    */

   ir->val->accept(this);
   src = this->result;
   assert(src.file != PROGRAM_UNDEFINED);
   assert(ir->type->vector_elements > 0);

   for (i = 0; i < 4; i++) {
      if (i < ir->type->vector_elements) {
	 switch (i) {
	 case 0:
	    swizzle[i] = GET_SWZ(src.swizzle, ir->mask.x);
	    break;
	 case 1:
	    swizzle[i] = GET_SWZ(src.swizzle, ir->mask.y);
	    break;
	 case 2:
	    swizzle[i] = GET_SWZ(src.swizzle, ir->mask.z);
	    break;
	 case 3:
	    swizzle[i] = GET_SWZ(src.swizzle, ir->mask.w);
	    break;
	 }
      } else {
	 /* If the type is smaller than a vec4, replicate the last
	  * channel out.
	  */
	 swizzle[i] = swizzle[ir->type->vector_elements - 1];
      }
   }

   src.swizzle = MAKE_SWIZZLE4(swizzle[0], swizzle[1], swizzle[2], swizzle[3]);

   this->result = src;
}

void
ir_to_mesa_visitor::visit(ir_dereference_variable *ir)
{
   variable_storage *entry = find_variable_storage(ir->var);
   ir_variable *var = ir->var;

   if (!entry) {
      switch (var->data.mode) {
      case ir_var_uniform:
	 entry = new(mem_ctx) variable_storage(var, PROGRAM_UNIFORM,
					       var->data.param_index);
	 this->variables.push_tail(entry);
	 break;
      case ir_var_shader_in:
	 /* The linker assigns locations for varyings and attributes,
	  * including deprecated builtins (like gl_Color),
	  * user-assigned generic attributes (glBindVertexLocation),
	  * and user-defined varyings.
	  */
	 assert(var->data.location != -1);
         entry = new(mem_ctx) variable_storage(var,
                                               PROGRAM_INPUT,
                                               var->data.location);
         break;
      case ir_var_shader_out:
	 assert(var->data.location != -1);
         entry = new(mem_ctx) variable_storage(var,
                                               PROGRAM_OUTPUT,
                                               var->data.location);
	 break;
      case ir_var_system_value:
         entry = new(mem_ctx) variable_storage(var,
                                               PROGRAM_SYSTEM_VALUE,
                                               var->data.location);
         break;
      case ir_var_auto:
      case ir_var_temporary:
	 entry = new(mem_ctx) variable_storage(var, PROGRAM_TEMPORARY,
					       this->next_temp);
	 this->variables.push_tail(entry);

	 next_temp += type_size(var->type);
	 break;
      }

      if (!entry) {
	 printf("Failed to make storage for %s\n", var->name);
	 exit(1);
      }
   }

   this->result = src_reg(entry->file, entry->index, var->type);
}

void
ir_to_mesa_visitor::visit(ir_dereference_array *ir)
{
   ir_constant *index;
   src_reg src;
   int element_size = type_size(ir->type);

   index = ir->array_index->constant_expression_value(ralloc_parent(ir));

   ir->array->accept(this);
   src = this->result;

   if (index) {
      src.index += index->value.i[0] * element_size;
   } else {
      /* Variable index array dereference.  It eats the "vec4" of the
       * base of the array and an index that offsets the Mesa register
       * index.
       */
      ir->array_index->accept(this);

      src_reg index_reg;

      if (element_size == 1) {
	 index_reg = this->result;
      } else {
	 index_reg = get_temp(glsl_type::float_type);

	 emit(ir, OPCODE_MUL, dst_reg(index_reg),
	      this->result, src_reg_for_float(element_size));
      }

      /* If there was already a relative address register involved, add the
       * new and the old together to get the new offset.
       */
      if (src.reladdr != NULL)  {
	 src_reg accum_reg = get_temp(glsl_type::float_type);

	 emit(ir, OPCODE_ADD, dst_reg(accum_reg),
	      index_reg, *src.reladdr);

	 index_reg = accum_reg;
      }

      src.reladdr = ralloc(mem_ctx, src_reg);
      memcpy(src.reladdr, &index_reg, sizeof(index_reg));
   }

   /* If the type is smaller than a vec4, replicate the last channel out. */
   if (ir->type->is_scalar() || ir->type->is_vector())
      src.swizzle = swizzle_for_size(ir->type->vector_elements);
   else
      src.swizzle = SWIZZLE_NOOP;

   this->result = src;
}

void
ir_to_mesa_visitor::visit(ir_dereference_record *ir)
{
   unsigned int i;
   const glsl_type *struct_type = ir->record->type;
   int offset = 0;

   ir->record->accept(this);

   assert(ir->field_idx >= 0);
   for (i = 0; i < struct_type->length; i++) {
      if (i == (unsigned) ir->field_idx)
	 break;
      offset += type_size(struct_type->fields.structure[i].type);
   }

   /* If the type is smaller than a vec4, replicate the last channel out. */
   if (ir->type->is_scalar() || ir->type->is_vector())
      this->result.swizzle = swizzle_for_size(ir->type->vector_elements);
   else
      this->result.swizzle = SWIZZLE_NOOP;

   this->result.index += offset;
}

/**
 * We want to be careful in assignment setup to hit the actual storage
 * instead of potentially using a temporary like we might with the
 * ir_dereference handler.
 */
static dst_reg
get_assignment_lhs(ir_dereference *ir, ir_to_mesa_visitor *v)
{
   /* The LHS must be a dereference.  If the LHS is a variable indexed array
    * access of a vector, it must be separated into a series conditional moves
    * before reaching this point (see ir_vec_index_to_cond_assign).
    */
   assert(ir->as_dereference());
   ir_dereference_array *deref_array = ir->as_dereference_array();
   if (deref_array) {
      assert(!deref_array->array->type->is_vector());
   }

   /* Use the rvalue deref handler for the most part.  We'll ignore
    * swizzles in it and write swizzles using writemask, though.
    */
   ir->accept(v);
   return dst_reg(v->result);
}

/* Calculate the sampler index and also calculate the base uniform location
 * for struct members.
 */
static void
calc_sampler_offsets(struct gl_shader_program *prog, ir_dereference *deref,
                     unsigned *offset, unsigned *array_elements,
                     unsigned *location)
{
   if (deref->ir_type == ir_type_dereference_variable)
      return;

   switch (deref->ir_type) {
   case ir_type_dereference_array: {
      ir_dereference_array *deref_arr = deref->as_dereference_array();

      void *mem_ctx = ralloc_parent(deref_arr);
      ir_constant *array_index =
         deref_arr->array_index->constant_expression_value(mem_ctx);

      if (!array_index) {
	 /* GLSL 1.10 and 1.20 allowed variable sampler array indices,
	  * while GLSL 1.30 requires that the array indices be
	  * constant integer expressions.  We don't expect any driver
	  * to actually work with a really variable array index, so
	  * all that would work would be an unrolled loop counter that ends
	  * up being constant above.
	  */
         ralloc_strcat(&prog->data->InfoLog,
		       "warning: Variable sampler array index unsupported.\n"
		       "This feature of the language was removed in GLSL 1.20 "
		       "and is unlikely to be supported for 1.10 in Mesa.\n");
      } else {
         *offset += array_index->value.u[0] * *array_elements;
      }

      *array_elements *= deref_arr->array->type->length;

      calc_sampler_offsets(prog, deref_arr->array->as_dereference(),
                           offset, array_elements, location);
      break;
   }

   case ir_type_dereference_record: {
      ir_dereference_record *deref_record = deref->as_dereference_record();
      unsigned field_index = deref_record->field_idx;
      *location +=
         deref_record->record->type->struct_location_offset(field_index);
      calc_sampler_offsets(prog, deref_record->record->as_dereference(),
                           offset, array_elements, location);
      break;
   }

   default:
      unreachable("Invalid deref type");
      break;
   }
}

static int
get_sampler_uniform_value(class ir_dereference *sampler,
                          struct gl_shader_program *shader_program,
                          const struct gl_program *prog)
{
   GLuint shader = _mesa_program_enum_to_shader_stage(prog->Target);
   ir_variable *var = sampler->variable_referenced();
   unsigned location = var->data.location;
   unsigned array_elements = 1;
   unsigned offset = 0;

   calc_sampler_offsets(shader_program, sampler, &offset, &array_elements,
                        &location);

   assert(shader_program->data->UniformStorage[location].opaque[shader].active);
   return shader_program->data->UniformStorage[location].opaque[shader].index +
          offset;
}

/**
 * Process the condition of a conditional assignment
 *
 * Examines the condition of a conditional assignment to generate the optimal
 * first operand of a \c CMP instruction.  If the condition is a relational
 * operator with 0 (e.g., \c ir_binop_less), the value being compared will be
 * used as the source for the \c CMP instruction.  Otherwise the comparison
 * is processed to a boolean result, and the boolean result is used as the
 * operand to the CMP instruction.
 */
bool
ir_to_mesa_visitor::process_move_condition(ir_rvalue *ir)
{
   ir_rvalue *src_ir = ir;
   bool negate = true;
   bool switch_order = false;

   ir_expression *const expr = ir->as_expression();
   if ((expr != NULL) && (expr->num_operands == 2)) {
      bool zero_on_left = false;

      if (expr->operands[0]->is_zero()) {
	 src_ir = expr->operands[1];
	 zero_on_left = true;
      } else if (expr->operands[1]->is_zero()) {
	 src_ir = expr->operands[0];
	 zero_on_left = false;
      }

      /*      a is -  0  +            -  0  +
       * (a <  0)  T  F  F  ( a < 0)  T  F  F
       * (0 <  a)  F  F  T  (-a < 0)  F  F  T
       * (a >= 0)  F  T  T  ( a < 0)  T  F  F  (swap order of other operands)
       * (0 >= a)  T  T  F  (-a < 0)  F  F  T  (swap order of other operands)
       *
       * Note that exchanging the order of 0 and 'a' in the comparison simply
       * means that the value of 'a' should be negated.
       */
      if (src_ir != ir) {
	 switch (expr->operation) {
	 case ir_binop_less:
	    switch_order = false;
	    negate = zero_on_left;
	    break;

	 case ir_binop_gequal:
	    switch_order = true;
	    negate = zero_on_left;
	    break;

	 default:
	    /* This isn't the right kind of comparison afterall, so make sure
	     * the whole condition is visited.
	     */
	    src_ir = ir;
	    break;
	 }
      }
   }

   src_ir->accept(this);

   /* We use the OPCODE_CMP (a < 0 ? b : c) for conditional moves, and the
    * condition we produced is 0.0 or 1.0.  By flipping the sign, we can
    * choose which value OPCODE_CMP produces without an extra instruction
    * computing the condition.
    */
   if (negate)
      this->result.negate = ~this->result.negate;

   return switch_order;
}

void
ir_to_mesa_visitor::visit(ir_assignment *ir)
{
   dst_reg l;
   src_reg r;
   int i;

   ir->rhs->accept(this);
   r = this->result;

   l = get_assignment_lhs(ir->lhs, this);

   /* FINISHME: This should really set to the correct maximal writemask for each
    * FINISHME: component written (in the loops below).  This case can only
    * FINISHME: occur for matrices, arrays, and structures.
    */
   if (ir->write_mask == 0) {
      assert(!ir->lhs->type->is_scalar() && !ir->lhs->type->is_vector());
      l.writemask = WRITEMASK_XYZW;
   } else if (ir->lhs->type->is_scalar()) {
      /* FINISHME: This hack makes writing to gl_FragDepth, which lives in the
       * FINISHME: W component of fragment shader output zero, work correctly.
       */
      l.writemask = WRITEMASK_XYZW;
   } else {
      int swizzles[4];
      int first_enabled_chan = 0;
      int rhs_chan = 0;

      assert(ir->lhs->type->is_vector());
      l.writemask = ir->write_mask;

      for (int i = 0; i < 4; i++) {
	 if (l.writemask & (1 << i)) {
	    first_enabled_chan = GET_SWZ(r.swizzle, i);
	    break;
	 }
      }

      /* Swizzle a small RHS vector into the channels being written.
       *
       * glsl ir treats write_mask as dictating how many channels are
       * present on the RHS while Mesa IR treats write_mask as just
       * showing which channels of the vec4 RHS get written.
       */
      for (int i = 0; i < 4; i++) {
	 if (l.writemask & (1 << i))
	    swizzles[i] = GET_SWZ(r.swizzle, rhs_chan++);
	 else
	    swizzles[i] = first_enabled_chan;
      }
      r.swizzle = MAKE_SWIZZLE4(swizzles[0], swizzles[1],
				swizzles[2], swizzles[3]);
   }

   assert(l.file != PROGRAM_UNDEFINED);
   assert(r.file != PROGRAM_UNDEFINED);

   if (ir->condition) {
      const bool switch_order = this->process_move_condition(ir->condition);
      src_reg condition = this->result;

      for (i = 0; i < type_size(ir->lhs->type); i++) {
	 if (switch_order) {
	    emit(ir, OPCODE_CMP, l, condition, src_reg(l), r);
	 } else {
	    emit(ir, OPCODE_CMP, l, condition, r, src_reg(l));
	 }

	 l.index++;
	 r.index++;
      }
   } else {
      for (i = 0; i < type_size(ir->lhs->type); i++) {
	 emit(ir, OPCODE_MOV, l, r);
	 l.index++;
	 r.index++;
      }
   }
}


void
ir_to_mesa_visitor::visit(ir_constant *ir)
{
   src_reg src;
   GLfloat stack_vals[4] = { 0 };
   GLfloat *values = stack_vals;
   unsigned int i;

   /* Unfortunately, 4 floats is all we can get into
    * _mesa_add_unnamed_constant.  So, make a temp to store an
    * aggregate constant and move each constant value into it.  If we
    * get lucky, copy propagation will eliminate the extra moves.
    */

   if (ir->type->is_struct()) {
      src_reg temp_base = get_temp(ir->type);
      dst_reg temp = dst_reg(temp_base);

      for (i = 0; i < ir->type->length; i++) {
         ir_constant *const field_value = ir->get_record_field(i);
	 int size = type_size(field_value->type);

	 assert(size > 0);

	 field_value->accept(this);
	 src = this->result;

         for (unsigned j = 0; j < (unsigned int)size; j++) {
	    emit(ir, OPCODE_MOV, temp, src);

	    src.index++;
	    temp.index++;
	 }
      }
      this->result = temp_base;
      return;
   }

   if (ir->type->is_array()) {
      src_reg temp_base = get_temp(ir->type);
      dst_reg temp = dst_reg(temp_base);
      int size = type_size(ir->type->fields.array);

      assert(size > 0);

      for (i = 0; i < ir->type->length; i++) {
	 ir->const_elements[i]->accept(this);
	 src = this->result;
	 for (int j = 0; j < size; j++) {
	    emit(ir, OPCODE_MOV, temp, src);

	    src.index++;
	    temp.index++;
	 }
      }
      this->result = temp_base;
      return;
   }

   if (ir->type->is_matrix()) {
      src_reg mat = get_temp(ir->type);
      dst_reg mat_column = dst_reg(mat);

      for (i = 0; i < ir->type->matrix_columns; i++) {
	 assert(ir->type->is_float());
	 values = &ir->value.f[i * ir->type->vector_elements];

	 src = src_reg(PROGRAM_CONSTANT, -1, NULL);
	 src.index = _mesa_add_unnamed_constant(this->prog->Parameters,
						(gl_constant_value *) values,
						ir->type->vector_elements,
						&src.swizzle);
	 emit(ir, OPCODE_MOV, mat_column, src);

	 mat_column.index++;
      }

      this->result = mat;
      return;
   }

   src.file = PROGRAM_CONSTANT;
   switch (ir->type->base_type) {
   case GLSL_TYPE_FLOAT:
      values = &ir->value.f[0];
      break;
   case GLSL_TYPE_UINT:
      for (i = 0; i < ir->type->vector_elements; i++) {
	 values[i] = ir->value.u[i];
      }
      break;
   case GLSL_TYPE_INT:
      for (i = 0; i < ir->type->vector_elements; i++) {
	 values[i] = ir->value.i[i];
      }
      break;
   case GLSL_TYPE_BOOL:
      for (i = 0; i < ir->type->vector_elements; i++) {
	 values[i] = ir->value.b[i];
      }
      break;
   default:
      assert(!"Non-float/uint/int/bool constant");
   }

   this->result = src_reg(PROGRAM_CONSTANT, -1, ir->type);
   this->result.index = _mesa_add_unnamed_constant(this->prog->Parameters,
						   (gl_constant_value *) values,
						   ir->type->vector_elements,
						   &this->result.swizzle);
}

void
ir_to_mesa_visitor::visit(ir_call *)
{
   assert(!"ir_to_mesa: All function calls should have been inlined by now.");
}

void
ir_to_mesa_visitor::visit(ir_texture *ir)
{
   src_reg result_src, coord, lod_info, projector, dx, dy;
   dst_reg result_dst, coord_dst;
   ir_to_mesa_instruction *inst = NULL;
   prog_opcode opcode = OPCODE_NOP;

   if (ir->op == ir_txs)
      this->result = src_reg_for_float(0.0);
   else
      ir->coordinate->accept(this);

   /* Put our coords in a temp.  We'll need to modify them for shadow,
    * projection, or LOD, so the only case we'd use it as-is is if
    * we're doing plain old texturing.  Mesa IR optimization should
    * handle cleaning up our mess in that case.
    */
   coord = get_temp(glsl_type::vec4_type);
   coord_dst = dst_reg(coord);
   emit(ir, OPCODE_MOV, coord_dst, this->result);

   if (ir->projector) {
      ir->projector->accept(this);
      projector = this->result;
   }

   /* Storage for our result.  Ideally for an assignment we'd be using
    * the actual storage for the result here, instead.
    */
   result_src = get_temp(glsl_type::vec4_type);
   result_dst = dst_reg(result_src);

   switch (ir->op) {
   case ir_tex:
   case ir_txs:
      opcode = OPCODE_TEX;
      break;
   case ir_txb:
      opcode = OPCODE_TXB;
      ir->lod_info.bias->accept(this);
      lod_info = this->result;
      break;
   case ir_txf:
      /* Pretend to be TXL so the sampler, coordinate, lod are available */
   case ir_txl:
      opcode = OPCODE_TXL;
      ir->lod_info.lod->accept(this);
      lod_info = this->result;
      break;
   case ir_txd:
      opcode = OPCODE_TXD;
      ir->lod_info.grad.dPdx->accept(this);
      dx = this->result;
      ir->lod_info.grad.dPdy->accept(this);
      dy = this->result;
      break;
   case ir_txf_ms:
      assert(!"Unexpected ir_txf_ms opcode");
      break;
   case ir_lod:
      assert(!"Unexpected ir_lod opcode");
      break;
   case ir_tg4:
      assert(!"Unexpected ir_tg4 opcode");
      break;
   case ir_query_levels:
      assert(!"Unexpected ir_query_levels opcode");
      break;
   case ir_samples_identical:
      unreachable("Unexpected ir_samples_identical opcode");
   case ir_texture_samples:
      unreachable("Unexpected ir_texture_samples opcode");
   }

   const glsl_type *sampler_type = ir->sampler->type;

   if (ir->projector) {
      if (opcode == OPCODE_TEX) {
	 /* Slot the projector in as the last component of the coord. */
	 coord_dst.writemask = WRITEMASK_W;
	 emit(ir, OPCODE_MOV, coord_dst, projector);
	 coord_dst.writemask = WRITEMASK_XYZW;
	 opcode = OPCODE_TXP;
      } else {
	 src_reg coord_w = coord;
	 coord_w.swizzle = SWIZZLE_WWWW;

	 /* For the other TEX opcodes there's no projective version
	  * since the last slot is taken up by lod info.  Do the
	  * projective divide now.
	  */
	 coord_dst.writemask = WRITEMASK_W;
	 emit(ir, OPCODE_RCP, coord_dst, projector);

	 /* In the case where we have to project the coordinates "by hand,"
	  * the shadow comparator value must also be projected.
	  */
	 src_reg tmp_src = coord;
	 if (ir->shadow_comparator) {
	    /* Slot the shadow value in as the second to last component of the
	     * coord.
	     */
	    ir->shadow_comparator->accept(this);

	    tmp_src = get_temp(glsl_type::vec4_type);
	    dst_reg tmp_dst = dst_reg(tmp_src);

	    /* Projective division not allowed for array samplers. */
	    assert(!sampler_type->sampler_array);

	    tmp_dst.writemask = WRITEMASK_Z;
	    emit(ir, OPCODE_MOV, tmp_dst, this->result);

	    tmp_dst.writemask = WRITEMASK_XY;
	    emit(ir, OPCODE_MOV, tmp_dst, coord);
	 }

	 coord_dst.writemask = WRITEMASK_XYZ;
	 emit(ir, OPCODE_MUL, coord_dst, tmp_src, coord_w);

	 coord_dst.writemask = WRITEMASK_XYZW;
	 coord.swizzle = SWIZZLE_XYZW;
      }
   }

   /* If projection is done and the opcode is not OPCODE_TXP, then the shadow
    * comparator was put in the correct place (and projected) by the code,
    * above, that handles by-hand projection.
    */
   if (ir->shadow_comparator && (!ir->projector || opcode == OPCODE_TXP)) {
      /* Slot the shadow value in as the second to last component of the
       * coord.
       */
      ir->shadow_comparator->accept(this);

      /* XXX This will need to be updated for cubemap array samplers. */
      if (sampler_type->sampler_dimensionality == GLSL_SAMPLER_DIM_2D &&
          sampler_type->sampler_array) {
         coord_dst.writemask = WRITEMASK_W;
      } else {
         coord_dst.writemask = WRITEMASK_Z;
      }

      emit(ir, OPCODE_MOV, coord_dst, this->result);
      coord_dst.writemask = WRITEMASK_XYZW;
   }

   if (opcode == OPCODE_TXL || opcode == OPCODE_TXB) {
      /* Mesa IR stores lod or lod bias in the last channel of the coords. */
      coord_dst.writemask = WRITEMASK_W;
      emit(ir, OPCODE_MOV, coord_dst, lod_info);
      coord_dst.writemask = WRITEMASK_XYZW;
   }

   if (opcode == OPCODE_TXD)
      inst = emit(ir, opcode, result_dst, coord, dx, dy);
   else
      inst = emit(ir, opcode, result_dst, coord);

   if (ir->shadow_comparator)
      inst->tex_shadow = GL_TRUE;

   inst->sampler = get_sampler_uniform_value(ir->sampler, shader_program,
                                             prog);

   switch (sampler_type->sampler_dimensionality) {
   case GLSL_SAMPLER_DIM_1D:
      inst->tex_target = (sampler_type->sampler_array)
	 ? TEXTURE_1D_ARRAY_INDEX : TEXTURE_1D_INDEX;
      break;
   case GLSL_SAMPLER_DIM_2D:
      inst->tex_target = (sampler_type->sampler_array)
	 ? TEXTURE_2D_ARRAY_INDEX : TEXTURE_2D_INDEX;
      break;
   case GLSL_SAMPLER_DIM_3D:
      inst->tex_target = TEXTURE_3D_INDEX;
      break;
   case GLSL_SAMPLER_DIM_CUBE:
      inst->tex_target = TEXTURE_CUBE_INDEX;
      break;
   case GLSL_SAMPLER_DIM_RECT:
      inst->tex_target = TEXTURE_RECT_INDEX;
      break;
   case GLSL_SAMPLER_DIM_BUF:
      assert(!"FINISHME: Implement ARB_texture_buffer_object");
      break;
   case GLSL_SAMPLER_DIM_EXTERNAL:
      inst->tex_target = TEXTURE_EXTERNAL_INDEX;
      break;
   default:
      assert(!"Should not get here.");
   }

   this->result = result_src;
}

void
ir_to_mesa_visitor::visit(ir_return *ir)
{
   /* Non-void functions should have been inlined.  We may still emit RETs
    * from main() unless the EmitNoMainReturn option is set.
    */
   assert(!ir->get_value());
   emit(ir, OPCODE_RET);
}

void
ir_to_mesa_visitor::visit(ir_discard *ir)
{
   if (!ir->condition)
      ir->condition = new(mem_ctx) ir_constant(true);

   ir->condition->accept(this);
   this->result.negate = ~this->result.negate;
   emit(ir, OPCODE_KIL, undef_dst, this->result);
}

void
ir_to_mesa_visitor::visit(ir_demote *ir)
{
   assert(!"demote statement unsupported");
}

void
ir_to_mesa_visitor::visit(ir_if *ir)
{
   ir_to_mesa_instruction *if_inst;

   ir->condition->accept(this);
   assert(this->result.file != PROGRAM_UNDEFINED);

   if_inst = emit(ir->condition, OPCODE_IF, undef_dst, this->result);

   this->instructions.push_tail(if_inst);

   visit_exec_list(&ir->then_instructions, this);

   if (!ir->else_instructions.is_empty()) {
      emit(ir->condition, OPCODE_ELSE);
      visit_exec_list(&ir->else_instructions, this);
   }

   emit(ir->condition, OPCODE_ENDIF);
}

void
ir_to_mesa_visitor::visit(ir_emit_vertex *)
{
   assert(!"Geometry shaders not supported.");
}

void
ir_to_mesa_visitor::visit(ir_end_primitive *)
{
   assert(!"Geometry shaders not supported.");
}

void
ir_to_mesa_visitor::visit(ir_barrier *)
{
   unreachable("GLSL barrier() not supported.");
}

ir_to_mesa_visitor::ir_to_mesa_visitor()
{
   result.file = PROGRAM_UNDEFINED;
   next_temp = 1;
   next_signature_id = 1;
   current_function = NULL;
   mem_ctx = ralloc_context(NULL);
}

ir_to_mesa_visitor::~ir_to_mesa_visitor()
{
   ralloc_free(mem_ctx);
}

static struct prog_src_register
mesa_src_reg_from_ir_src_reg(src_reg reg)
{
   struct prog_src_register mesa_reg;

   mesa_reg.File = reg.file;
   assert(reg.index < (1 << INST_INDEX_BITS));
   mesa_reg.Index = reg.index;
   mesa_reg.Swizzle = reg.swizzle;
   mesa_reg.RelAddr = reg.reladdr != NULL;
   mesa_reg.Negate = reg.negate;

   return mesa_reg;
}

static void
set_branchtargets(ir_to_mesa_visitor *v,
		  struct prog_instruction *mesa_instructions,
		  int num_instructions)
{
   int if_count = 0, loop_count = 0;
   int *if_stack, *loop_stack;
   int if_stack_pos = 0, loop_stack_pos = 0;
   int i, j;

   for (i = 0; i < num_instructions; i++) {
      switch (mesa_instructions[i].Opcode) {
      case OPCODE_IF:
	 if_count++;
	 break;
      case OPCODE_BGNLOOP:
	 loop_count++;
	 break;
      case OPCODE_BRK:
      case OPCODE_CONT:
	 mesa_instructions[i].BranchTarget = -1;
	 break;
      default:
	 break;
      }
   }

   if_stack = rzalloc_array(v->mem_ctx, int, if_count);
   loop_stack = rzalloc_array(v->mem_ctx, int, loop_count);

   for (i = 0; i < num_instructions; i++) {
      switch (mesa_instructions[i].Opcode) {
      case OPCODE_IF:
	 if_stack[if_stack_pos] = i;
	 if_stack_pos++;
	 break;
      case OPCODE_ELSE:
	 mesa_instructions[if_stack[if_stack_pos - 1]].BranchTarget = i;
	 if_stack[if_stack_pos - 1] = i;
	 break;
      case OPCODE_ENDIF:
	 mesa_instructions[if_stack[if_stack_pos - 1]].BranchTarget = i;
	 if_stack_pos--;
	 break;
      case OPCODE_BGNLOOP:
	 loop_stack[loop_stack_pos] = i;
	 loop_stack_pos++;
	 break;
      case OPCODE_ENDLOOP:
	 loop_stack_pos--;
	 /* Rewrite any breaks/conts at this nesting level (haven't
	  * already had a BranchTarget assigned) to point to the end
	  * of the loop.
	  */
	 for (j = loop_stack[loop_stack_pos]; j < i; j++) {
	    if (mesa_instructions[j].Opcode == OPCODE_BRK ||
		mesa_instructions[j].Opcode == OPCODE_CONT) {
	       if (mesa_instructions[j].BranchTarget == -1) {
		  mesa_instructions[j].BranchTarget = i;
	       }
	    }
	 }
	 /* The loop ends point at each other. */
	 mesa_instructions[i].BranchTarget = loop_stack[loop_stack_pos];
	 mesa_instructions[loop_stack[loop_stack_pos]].BranchTarget = i;
	 break;
      case OPCODE_CAL:
	 foreach_in_list(function_entry, entry, &v->function_signatures) {
	    if (entry->sig_id == mesa_instructions[i].BranchTarget) {
	       mesa_instructions[i].BranchTarget = entry->inst;
	       break;
	    }
	 }
	 break;
      default:
	 break;
      }
   }
}

static void
print_program(struct prog_instruction *mesa_instructions,
	      ir_instruction **mesa_instruction_annotation,
	      int num_instructions)
{
   ir_instruction *last_ir = NULL;
   int i;
   int indent = 0;

   for (i = 0; i < num_instructions; i++) {
      struct prog_instruction *mesa_inst = mesa_instructions + i;
      ir_instruction *ir = mesa_instruction_annotation[i];

      fprintf(stdout, "%3d: ", i);

      if (last_ir != ir && ir) {
	 int j;

	 for (j = 0; j < indent; j++) {
	    fprintf(stdout, " ");
	 }
	 ir->print();
	 printf("\n");
	 last_ir = ir;

	 fprintf(stdout, "     "); /* line number spacing. */
      }

      indent = _mesa_fprint_instruction_opt(stdout, mesa_inst, indent,
					    PROG_PRINT_DEBUG, NULL);
   }
}

namespace {

class add_uniform_to_shader : public program_resource_visitor {
public:
   add_uniform_to_shader(struct gl_context *ctx,
                         struct gl_shader_program *shader_program,
			 struct gl_program_parameter_list *params)
      : ctx(ctx), shader_program(shader_program), params(params), idx(-1)
   {
      /* empty */
   }

   void process(ir_variable *var)
   {
      this->idx = -1;
      this->var = var;
      this->program_resource_visitor::process(var,
                                         ctx->Const.UseSTD430AsDefaultPacking);
      var->data.param_index = this->idx;
   }

private:
   virtual void visit_field(const glsl_type *type, const char *name,
                            bool row_major, const glsl_type *record_type,
                            const enum glsl_interface_packing packing,
                            bool last_field);

   struct gl_context *ctx;
   struct gl_shader_program *shader_program;
   struct gl_program_parameter_list *params;
   int idx;
   ir_variable *var;
};

} /* anonymous namespace */

void
add_uniform_to_shader::visit_field(const glsl_type *type, const char *name,
                                   bool /* row_major */,
                                   const glsl_type * /* record_type */,
                                   const enum glsl_interface_packing,
                                   bool /* last_field */)
{
   /* opaque types don't use storage in the param list unless they are
    * bindless samplers or images.
    */
   if (type->contains_opaque() && !var->data.bindless)
      return;

   /* Add the uniform to the param list */
   assert(_mesa_lookup_parameter_index(params, name) < 0);
   int index = _mesa_lookup_parameter_index(params, name);

   unsigned num_params = type->arrays_of_arrays_size();
   num_params = MAX2(num_params, 1);
   num_params *= type->without_array()->matrix_columns;

   bool is_dual_slot = type->without_array()->is_dual_slot();
   if (is_dual_slot)
      num_params *= 2;

   _mesa_reserve_parameter_storage(params, num_params);
   index = params->NumParameters;

   if (ctx->Const.PackedDriverUniformStorage) {
      for (unsigned i = 0; i < num_params; i++) {
         unsigned dmul = type->without_array()->is_64bit() ? 2 : 1;
         unsigned comps = type->without_array()->vector_elements * dmul;
         if (is_dual_slot) {
            if (i & 0x1)
               comps -= 4;
            else
               comps = 4;
         }

         _mesa_add_parameter(params, PROGRAM_UNIFORM, name, comps,
                             type->gl_type, NULL, NULL, false);
      }
   } else {
      for (unsigned i = 0; i < num_params; i++) {
         _mesa_add_parameter(params, PROGRAM_UNIFORM, name, 4,
                             type->gl_type, NULL, NULL, true);
      }
   }

   /* The first part of the uniform that's processed determines the base
    * location of the whole uniform (for structures).
    */
   if (this->idx < 0)
      this->idx = index;

   /* Each Parameter will hold the index to the backing uniform storage.
    * This avoids relying on names to match parameters and uniform
    * storages later when associating uniform storage.
    */
   unsigned location;
   const bool found =
      shader_program->UniformHash->get(location, params->Parameters[index].Name);
   assert(found);

   for (unsigned i = 0; i < num_params; i++) {
      struct gl_program_parameter *param = &params->Parameters[index + i];
      param->UniformStorageIndex = location;
      param->MainUniformStorageIndex = params->Parameters[this->idx].UniformStorageIndex;
   }
}

/**
 * Generate the program parameters list for the user uniforms in a shader
 *
 * \param shader_program Linked shader program.  This is only used to
 *                       emit possible link errors to the info log.
 * \param sh             Shader whose uniforms are to be processed.
 * \param params         Parameter list to be filled in.
 */
void
_mesa_generate_parameters_list_for_uniforms(struct gl_context *ctx,
                                            struct gl_shader_program
					    *shader_program,
					    struct gl_linked_shader *sh,
					    struct gl_program_parameter_list
					    *params)
{
   add_uniform_to_shader add(ctx, shader_program, params);

   foreach_in_list(ir_instruction, node, sh->ir) {
      ir_variable *var = node->as_variable();

      if ((var == NULL) || (var->data.mode != ir_var_uniform)
	  || var->is_in_buffer_block() || (strncmp(var->name, "gl_", 3) == 0))
	 continue;

      add.process(var);
   }
}

void
_mesa_associate_uniform_storage(struct gl_context *ctx,
                                struct gl_shader_program *shader_program,
                                struct gl_program *prog)
{
   struct gl_program_parameter_list *params = prog->Parameters;
   gl_shader_stage shader_type = prog->info.stage;

   /* After adding each uniform to the parameter list, connect the storage for
    * the parameter with the tracking structure used by the API for the
    * uniform.
    */
   unsigned last_location = unsigned(~0);
   for (unsigned i = 0; i < params->NumParameters; i++) {
      if (params->Parameters[i].Type != PROGRAM_UNIFORM)
         continue;

      unsigned location = params->Parameters[i].UniformStorageIndex;

      struct gl_uniform_storage *storage =
         &shader_program->data->UniformStorage[location];

      /* Do not associate any uniform storage to built-in uniforms */
      if (storage->builtin)
         continue;

      if (location != last_location) {
         enum gl_uniform_driver_format format = uniform_native;
         unsigned columns = 0;

         int dmul;
         if (ctx->Const.PackedDriverUniformStorage && !prog->is_arb_asm) {
            dmul = storage->type->vector_elements * sizeof(float);
         } else {
            dmul = 4 * sizeof(float);
         }

         switch (storage->type->base_type) {
         case GLSL_TYPE_UINT64:
            if (storage->type->vector_elements > 2)
               dmul *= 2;
            /* fallthrough */
         case GLSL_TYPE_UINT:
         case GLSL_TYPE_UINT16:
         case GLSL_TYPE_UINT8:
            assert(ctx->Const.NativeIntegers);
            format = uniform_native;
            columns = 1;
            break;
         case GLSL_TYPE_INT64:
            if (storage->type->vector_elements > 2)
               dmul *= 2;
            /* fallthrough */
         case GLSL_TYPE_INT:
         case GLSL_TYPE_INT16:
         case GLSL_TYPE_INT8:
            format =
               (ctx->Const.NativeIntegers) ? uniform_native : uniform_int_float;
            columns = 1;
            break;
         case GLSL_TYPE_DOUBLE:
            if (storage->type->vector_elements > 2)
               dmul *= 2;
            /* fallthrough */
         case GLSL_TYPE_FLOAT:
         case GLSL_TYPE_FLOAT16:
            format = uniform_native;
            columns = storage->type->matrix_columns;
            break;
         case GLSL_TYPE_BOOL:
            format = uniform_native;
            columns = 1;
            break;
         case GLSL_TYPE_SAMPLER:
         case GLSL_TYPE_IMAGE:
         case GLSL_TYPE_SUBROUTINE:
            format = uniform_native;
            columns = 1;
            break;
         case GLSL_TYPE_ATOMIC_UINT:
         case GLSL_TYPE_ARRAY:
         case GLSL_TYPE_VOID:
         case GLSL_TYPE_STRUCT:
         case GLSL_TYPE_ERROR:
         case GLSL_TYPE_INTERFACE:
         case GLSL_TYPE_FUNCTION:
            assert(!"Should not get here.");
            break;
         }

         unsigned pvo = params->ParameterValueOffset[i];
         _mesa_uniform_attach_driver_storage(storage, dmul * columns, dmul,
                                             format,
                                             &params->ParameterValues[pvo]);

         /* When a bindless sampler/image is bound to a texture/image unit, we
          * have to overwrite the constant value by the resident handle
          * directly in the constant buffer before the next draw. One solution
          * is to keep track a pointer to the base of the data.
          */
         if (storage->is_bindless && (prog->sh.NumBindlessSamplers ||
                                      prog->sh.NumBindlessImages)) {
            unsigned array_elements = MAX2(1, storage->array_elements);

            for (unsigned j = 0; j < array_elements; ++j) {
               unsigned unit = storage->opaque[shader_type].index + j;

               if (storage->type->without_array()->is_sampler()) {
                  assert(unit >= 0 && unit < prog->sh.NumBindlessSamplers);
                  prog->sh.BindlessSamplers[unit].data =
                     &params->ParameterValues[pvo] + 4 * j;
               } else if (storage->type->without_array()->is_image()) {
                  assert(unit >= 0 && unit < prog->sh.NumBindlessImages);
                  prog->sh.BindlessImages[unit].data =
                     &params->ParameterValues[pvo] + 4 * j;
               }
            }
         }

         /* After attaching the driver's storage to the uniform, propagate any
          * data from the linker's backing store.  This will cause values from
          * initializers in the source code to be copied over.
          */
         unsigned array_elements = MAX2(1, storage->array_elements);
         if (ctx->Const.PackedDriverUniformStorage && !prog->is_arb_asm &&
             (storage->is_bindless || !storage->type->contains_opaque())) {
            const int dmul = storage->type->is_64bit() ? 2 : 1;
            const unsigned components =
               storage->type->vector_elements *
               storage->type->matrix_columns;

            for (unsigned s = 0; s < storage->num_driver_storage; s++) {
               gl_constant_value *uni_storage = (gl_constant_value *)
                  storage->driver_storage[s].data;
               memcpy(uni_storage, storage->storage,
                      sizeof(storage->storage[0]) * components *
                      array_elements * dmul);
            }
         } else {
            _mesa_propagate_uniforms_to_driver_storage(storage, 0,
                                                       array_elements);
         }

	      last_location = location;
      }
   }
}

/*
 * On a basic block basis, tracks available PROGRAM_TEMPORARY register
 * channels for copy propagation and updates following instructions to
 * use the original versions.
 *
 * The ir_to_mesa_visitor lazily produces code assuming that this pass
 * will occur.  As an example, a TXP production before this pass:
 *
 * 0: MOV TEMP[1], INPUT[4].xyyy;
 * 1: MOV TEMP[1].w, INPUT[4].wwww;
 * 2: TXP TEMP[2], TEMP[1], texture[0], 2D;
 *
 * and after:
 *
 * 0: MOV TEMP[1], INPUT[4].xyyy;
 * 1: MOV TEMP[1].w, INPUT[4].wwww;
 * 2: TXP TEMP[2], INPUT[4].xyyw, texture[0], 2D;
 *
 * which allows for dead code elimination on TEMP[1]'s writes.
 */
void
ir_to_mesa_visitor::copy_propagate(void)
{
   ir_to_mesa_instruction **acp = rzalloc_array(mem_ctx,
						    ir_to_mesa_instruction *,
						    this->next_temp * 4);
   int *acp_level = rzalloc_array(mem_ctx, int, this->next_temp * 4);
   int level = 0;

   foreach_in_list(ir_to_mesa_instruction, inst, &this->instructions) {
      assert(inst->dst.file != PROGRAM_TEMPORARY
	     || inst->dst.index < this->next_temp);

      /* First, do any copy propagation possible into the src regs. */
      for (int r = 0; r < 3; r++) {
	 ir_to_mesa_instruction *first = NULL;
	 bool good = true;
	 int acp_base = inst->src[r].index * 4;

	 if (inst->src[r].file != PROGRAM_TEMPORARY ||
	     inst->src[r].reladdr)
	    continue;

	 /* See if we can find entries in the ACP consisting of MOVs
	  * from the same src register for all the swizzled channels
	  * of this src register reference.
	  */
	 for (int i = 0; i < 4; i++) {
	    int src_chan = GET_SWZ(inst->src[r].swizzle, i);
	    ir_to_mesa_instruction *copy_chan = acp[acp_base + src_chan];

	    if (!copy_chan) {
	       good = false;
	       break;
	    }

	    assert(acp_level[acp_base + src_chan] <= level);

	    if (!first) {
	       first = copy_chan;
	    } else {
	       if (first->src[0].file != copy_chan->src[0].file ||
		   first->src[0].index != copy_chan->src[0].index) {
		  good = false;
		  break;
	       }
	    }
	 }

	 if (good) {
	    /* We've now validated that we can copy-propagate to
	     * replace this src register reference.  Do it.
	     */
	    inst->src[r].file = first->src[0].file;
	    inst->src[r].index = first->src[0].index;

	    int swizzle = 0;
	    for (int i = 0; i < 4; i++) {
	       int src_chan = GET_SWZ(inst->src[r].swizzle, i);
	       ir_to_mesa_instruction *copy_inst = acp[acp_base + src_chan];
	       swizzle |= (GET_SWZ(copy_inst->src[0].swizzle, src_chan) <<
			   (3 * i));
	    }
	    inst->src[r].swizzle = swizzle;
	 }
      }

      switch (inst->op) {
      case OPCODE_BGNLOOP:
      case OPCODE_ENDLOOP:
	 /* End of a basic block, clear the ACP entirely. */
	 memset(acp, 0, sizeof(*acp) * this->next_temp * 4);
	 break;

      case OPCODE_IF:
	 ++level;
	 break;

      case OPCODE_ENDIF:
      case OPCODE_ELSE:
	 /* Clear all channels written inside the block from the ACP, but
	  * leaving those that were not touched.
	  */
	 for (int r = 0; r < this->next_temp; r++) {
	    for (int c = 0; c < 4; c++) {
	       if (!acp[4 * r + c])
		  continue;

	       if (acp_level[4 * r + c] >= level)
		  acp[4 * r + c] = NULL;
	    }
	 }
	 if (inst->op == OPCODE_ENDIF)
	    --level;
	 break;

      default:
	 /* Continuing the block, clear any written channels from
	  * the ACP.
	  */
	 if (inst->dst.file == PROGRAM_TEMPORARY && inst->dst.reladdr) {
	    /* Any temporary might be written, so no copy propagation
	     * across this instruction.
	     */
	    memset(acp, 0, sizeof(*acp) * this->next_temp * 4);
	 } else if (inst->dst.file == PROGRAM_OUTPUT &&
		    inst->dst.reladdr) {
	    /* Any output might be written, so no copy propagation
	     * from outputs across this instruction.
	     */
	    for (int r = 0; r < this->next_temp; r++) {
	       for (int c = 0; c < 4; c++) {
		  if (!acp[4 * r + c])
		     continue;

		  if (acp[4 * r + c]->src[0].file == PROGRAM_OUTPUT)
		     acp[4 * r + c] = NULL;
	       }
	    }
	 } else if (inst->dst.file == PROGRAM_TEMPORARY ||
		    inst->dst.file == PROGRAM_OUTPUT) {
	    /* Clear where it's used as dst. */
	    if (inst->dst.file == PROGRAM_TEMPORARY) {
	       for (int c = 0; c < 4; c++) {
		  if (inst->dst.writemask & (1 << c)) {
		     acp[4 * inst->dst.index + c] = NULL;
		  }
	       }
	    }

	    /* Clear where it's used as src. */
	    for (int r = 0; r < this->next_temp; r++) {
	       for (int c = 0; c < 4; c++) {
		  if (!acp[4 * r + c])
		     continue;

		  int src_chan = GET_SWZ(acp[4 * r + c]->src[0].swizzle, c);

		  if (acp[4 * r + c]->src[0].file == inst->dst.file &&
		      acp[4 * r + c]->src[0].index == inst->dst.index &&
		      inst->dst.writemask & (1 << src_chan))
		  {
		     acp[4 * r + c] = NULL;
		  }
	       }
	    }
	 }
	 break;
      }

      /* If this is a copy, add it to the ACP. */
      if (inst->op == OPCODE_MOV &&
	  inst->dst.file == PROGRAM_TEMPORARY &&
	  !(inst->dst.file == inst->src[0].file &&
	    inst->dst.index == inst->src[0].index) &&
	  !inst->dst.reladdr &&
	  !inst->saturate &&
	  !inst->src[0].reladdr &&
	  !inst->src[0].negate) {
	 for (int i = 0; i < 4; i++) {
	    if (inst->dst.writemask & (1 << i)) {
	       acp[4 * inst->dst.index + i] = inst;
	       acp_level[4 * inst->dst.index + i] = level;
	    }
	 }
      }
   }

   ralloc_free(acp_level);
   ralloc_free(acp);
}


/**
 * Convert a shader's GLSL IR into a Mesa gl_program.
 */
static struct gl_program *
get_mesa_program(struct gl_context *ctx,
                 struct gl_shader_program *shader_program,
		 struct gl_linked_shader *shader)
{
   ir_to_mesa_visitor v;
   struct prog_instruction *mesa_instructions, *mesa_inst;
   ir_instruction **mesa_instruction_annotation;
   int i;
   struct gl_program *prog;
   GLenum target = _mesa_shader_stage_to_program(shader->Stage);
   const char *target_string = _mesa_shader_stage_to_string(shader->Stage);
   struct gl_shader_compiler_options *options =
         &ctx->Const.ShaderCompilerOptions[shader->Stage];

   validate_ir_tree(shader->ir);

   prog = shader->Program;
   prog->Parameters = _mesa_new_parameter_list();
   v.ctx = ctx;
   v.prog = prog;
   v.shader_program = shader_program;
   v.options = options;

   _mesa_generate_parameters_list_for_uniforms(ctx, shader_program, shader,
					       prog->Parameters);

   /* Emit Mesa IR for main(). */
   visit_exec_list(shader->ir, &v);
   v.emit(NULL, OPCODE_END);

   prog->arb.NumTemporaries = v.next_temp;

   unsigned num_instructions = v.instructions.length();

   mesa_instructions = rzalloc_array(prog, struct prog_instruction,
                                     num_instructions);
   mesa_instruction_annotation = ralloc_array(v.mem_ctx, ir_instruction *,
					      num_instructions);

   v.copy_propagate();

   /* Convert ir_mesa_instructions into prog_instructions.
    */
   mesa_inst = mesa_instructions;
   i = 0;
   foreach_in_list(const ir_to_mesa_instruction, inst, &v.instructions) {
      mesa_inst->Opcode = inst->op;
      if (inst->saturate)
	 mesa_inst->Saturate = GL_TRUE;
      mesa_inst->DstReg.File = inst->dst.file;
      mesa_inst->DstReg.Index = inst->dst.index;
      mesa_inst->DstReg.WriteMask = inst->dst.writemask;
      mesa_inst->DstReg.RelAddr = inst->dst.reladdr != NULL;
      mesa_inst->SrcReg[0] = mesa_src_reg_from_ir_src_reg(inst->src[0]);
      mesa_inst->SrcReg[1] = mesa_src_reg_from_ir_src_reg(inst->src[1]);
      mesa_inst->SrcReg[2] = mesa_src_reg_from_ir_src_reg(inst->src[2]);
      mesa_inst->TexSrcUnit = inst->sampler;
      mesa_inst->TexSrcTarget = inst->tex_target;
      mesa_inst->TexShadow = inst->tex_shadow;
      mesa_instruction_annotation[i] = inst->ir;

      /* Set IndirectRegisterFiles. */
      if (mesa_inst->DstReg.RelAddr)
         prog->arb.IndirectRegisterFiles |= 1 << mesa_inst->DstReg.File;

      /* Update program's bitmask of indirectly accessed register files */
      for (unsigned src = 0; src < 3; src++)
         if (mesa_inst->SrcReg[src].RelAddr)
            prog->arb.IndirectRegisterFiles |= 1 << mesa_inst->SrcReg[src].File;

      switch (mesa_inst->Opcode) {
      case OPCODE_IF:
	 if (options->MaxIfDepth == 0) {
	    linker_warning(shader_program,
			   "Couldn't flatten if-statement.  "
			   "This will likely result in software "
			   "rasterization.\n");
	 }
	 break;
      case OPCODE_BGNLOOP:
	 if (options->EmitNoLoops) {
	    linker_warning(shader_program,
			   "Couldn't unroll loop.  "
			   "This will likely result in software "
			   "rasterization.\n");
	 }
	 break;
      case OPCODE_CONT:
	 if (options->EmitNoCont) {
	    linker_warning(shader_program,
			   "Couldn't lower continue-statement.  "
			   "This will likely result in software "
			   "rasterization.\n");
	 }
	 break;
      case OPCODE_ARL:
         prog->arb.NumAddressRegs = 1;
	 break;
      default:
	 break;
      }

      mesa_inst++;
      i++;

      if (!shader_program->data->LinkStatus)
         break;
   }

   if (!shader_program->data->LinkStatus) {
      goto fail_exit;
   }

   set_branchtargets(&v, mesa_instructions, num_instructions);

   if (ctx->_Shader->Flags & GLSL_DUMP) {
      fprintf(stderr, "\n");
      fprintf(stderr, "GLSL IR for linked %s program %d:\n", target_string,
	      shader_program->Name);
      _mesa_print_ir(stderr, shader->ir, NULL);
      fprintf(stderr, "\n");
      fprintf(stderr, "\n");
      fprintf(stderr, "Mesa IR for linked %s program %d:\n", target_string,
	      shader_program->Name);
      print_program(mesa_instructions, mesa_instruction_annotation,
		    num_instructions);
      fflush(stderr);
   }

   prog->arb.Instructions = mesa_instructions;
   prog->arb.NumInstructions = num_instructions;

   /* Setting this to NULL prevents a possible double free in the fail_exit
    * path (far below).
    */
   mesa_instructions = NULL;

   do_set_program_inouts(shader->ir, prog, shader->Stage);

   prog->ShadowSamplers = shader->shadow_samplers;
   prog->ExternalSamplersUsed = gl_external_samplers(prog);
   _mesa_update_shader_textures_used(shader_program, prog);

   /* Set the gl_FragDepth layout. */
   if (target == GL_FRAGMENT_PROGRAM_ARB) {
      prog->info.fs.depth_layout = shader_program->FragDepthLayout;
   }

   _mesa_optimize_program(prog, prog);

   /* This has to be done last.  Any operation that can cause
    * prog->ParameterValues to get reallocated (e.g., anything that adds a
    * program constant) has to happen before creating this linkage.
    */
   _mesa_associate_uniform_storage(ctx, shader_program, prog);
   if (!shader_program->data->LinkStatus) {
      goto fail_exit;
   }

   return prog;

fail_exit:
   ralloc_free(mesa_instructions);
   _mesa_reference_program(ctx, &shader->Program, NULL);
   return NULL;
}

extern "C" {

/**
 * Link a shader.
 * Called via ctx->Driver.LinkShader()
 * This actually involves converting GLSL IR into Mesa gl_programs with
 * code lowering and other optimizations.
 */
GLboolean
_mesa_ir_link_shader(struct gl_context *ctx, struct gl_shader_program *prog)
{
   assert(prog->data->LinkStatus);

   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++) {
      if (prog->_LinkedShaders[i] == NULL)
	 continue;

      bool progress;
      exec_list *ir = prog->_LinkedShaders[i]->ir;
      const struct gl_shader_compiler_options *options =
            &ctx->Const.ShaderCompilerOptions[prog->_LinkedShaders[i]->Stage];

      do {
	 progress = false;

	 /* Lowering */
	 do_mat_op_to_vec(ir);
	 lower_instructions(ir, (MOD_TO_FLOOR | DIV_TO_MUL_RCP | EXP_TO_EXP2
				 | LOG_TO_LOG2 | INT_DIV_TO_MUL_RCP
				 | MUL64_TO_MUL_AND_MUL_HIGH
				 | ((options->EmitNoPow) ? POW_TO_EXP2 : 0)));

	 progress = do_common_optimization(ir, true, true,
                                           options, ctx->Const.NativeIntegers)
	   || progress;

	 progress = lower_quadop_vector(ir, true) || progress;

	 if (options->MaxIfDepth == 0)
	    progress = lower_discard(ir) || progress;

	 progress = lower_if_to_cond_assign((gl_shader_stage)i, ir,
                                            options->MaxIfDepth) || progress;

         progress = lower_noise(ir) || progress;

	 /* If there are forms of indirect addressing that the driver
	  * cannot handle, perform the lowering pass.
	  */
	 if (options->EmitNoIndirectInput || options->EmitNoIndirectOutput
	     || options->EmitNoIndirectTemp || options->EmitNoIndirectUniform)
	   progress =
	     lower_variable_index_to_cond_assign(prog->_LinkedShaders[i]->Stage, ir,
						 options->EmitNoIndirectInput,
						 options->EmitNoIndirectOutput,
						 options->EmitNoIndirectTemp,
						 options->EmitNoIndirectUniform)
	     || progress;

	 progress = do_vec_index_to_cond_assign(ir) || progress;
         progress = lower_vector_insert(ir, true) || progress;
      } while (progress);

      validate_ir_tree(ir);
   }

   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++) {
      struct gl_program *linked_prog;

      if (prog->_LinkedShaders[i] == NULL)
	 continue;

      linked_prog = get_mesa_program(ctx, prog, prog->_LinkedShaders[i]);

      if (linked_prog) {
         _mesa_copy_linked_program_data(prog, prog->_LinkedShaders[i]);

         if (!ctx->Driver.ProgramStringNotify(ctx,
                                              _mesa_shader_stage_to_program(i),
                                              linked_prog)) {
            _mesa_reference_program(ctx, &prog->_LinkedShaders[i]->Program,
                                    NULL);
            return GL_FALSE;
         }
      }
   }

   build_program_resource_list(ctx, prog, false);
   return prog->data->LinkStatus;
}

/**
 * Link a GLSL shader program.  Called via glLinkProgram().
 */
void
_mesa_glsl_link_shader(struct gl_context *ctx, struct gl_shader_program *prog)
{
   unsigned int i;
   bool spirv = false;

   _mesa_clear_shader_program_data(ctx, prog);

   prog->data = _mesa_create_shader_program_data();

   prog->data->LinkStatus = LINKING_SUCCESS;

   for (i = 0; i < prog->NumShaders; i++) {
      if (!prog->Shaders[i]->CompileStatus) {
	 linker_error(prog, "linking with uncompiled/unspecialized shader");
      }

      if (!i) {
         spirv = (prog->Shaders[i]->spirv_data != NULL);
      } else if (spirv && !prog->Shaders[i]->spirv_data) {
         /* The GL_ARB_gl_spirv spec adds a new bullet point to the list of
          * reasons LinkProgram can fail:
          *
          *    "All the shader objects attached to <program> do not have the
          *     same value for the SPIR_V_BINARY_ARB state."
          */
         linker_error(prog,
                      "not all attached shaders have the same "
                      "SPIR_V_BINARY_ARB state");
      }
   }
   prog->data->spirv = spirv;

   if (prog->data->LinkStatus) {
      if (!spirv)
         link_shaders(ctx, prog);
      else
         _mesa_spirv_link_shaders(ctx, prog);
   }

   /* If LinkStatus is LINKING_SUCCESS, then reset sampler validated to true.
    * Validation happens via the LinkShader call below. If LinkStatus is
    * LINKING_SKIPPED, then SamplersValidated will have been restored from the
    * shader cache.
    */
   if (prog->data->LinkStatus == LINKING_SUCCESS) {
      prog->SamplersValidated = GL_TRUE;
   }

   if (prog->data->LinkStatus && !ctx->Driver.LinkShader(ctx, prog)) {
      prog->data->LinkStatus = LINKING_FAILURE;
   }

   if (prog->data->LinkStatus != LINKING_FAILURE)
      _mesa_create_program_resource_hash(prog);

   /* Return early if we are loading the shader from on-disk cache */
   if (prog->data->LinkStatus == LINKING_SKIPPED)
      return;

   if (ctx->_Shader->Flags & GLSL_DUMP) {
      if (!prog->data->LinkStatus) {
	 fprintf(stderr, "GLSL shader program %d failed to link\n", prog->Name);
      }

      if (prog->data->InfoLog && prog->data->InfoLog[0] != 0) {
	 fprintf(stderr, "GLSL shader program %d info log:\n", prog->Name);
         fprintf(stderr, "%s\n", prog->data->InfoLog);
      }
   }

#ifdef ENABLE_SHADER_CACHE
   if (prog->data->LinkStatus)
      shader_cache_write_program_metadata(ctx, prog);
#endif
}

} /* extern "C" */
