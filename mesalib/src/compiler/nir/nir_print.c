/*
 * Copyright © 2014 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Connor Abbott (cwabbott0@gmail.com)
 *
 */

#include "nir.h"
#include "compiler/shader_enums.h"
#include "util/half_float.h"
#include "vulkan/vulkan_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h> /* for PRIx64 macro */

static void
print_tabs(unsigned num_tabs, FILE *fp)
{
   for (unsigned i = 0; i < num_tabs; i++)
      fprintf(fp, "\t");
}

typedef struct {
   FILE *fp;
   nir_shader *shader;
   /** map from nir_variable -> printable name */
   struct hash_table *ht;

   /** set of names used so far for nir_variables */
   struct set *syms;

   /* an index used to make new non-conflicting names */
   unsigned index;

   /**
    * Optional table of annotations mapping nir object
    * (such as instr or var) to message to print.
    */
   struct hash_table *annotations;
} print_state;

static void
print_annotation(print_state *state, void *obj)
{
   FILE *fp = state->fp;

   if (!state->annotations)
      return;

   struct hash_entry *entry = _mesa_hash_table_search(state->annotations, obj);
   if (!entry)
      return;

   const char *note = entry->data;
   _mesa_hash_table_remove(state->annotations, entry);

   fprintf(fp, "%s\n\n", note);
}

static void
print_register(nir_register *reg, print_state *state)
{
   FILE *fp = state->fp;
   if (reg->name != NULL)
      fprintf(fp, "/* %s */ ", reg->name);
   fprintf(fp, "r%u", reg->index);
}

static const char *sizes[] = { "error", "vec1", "vec2", "vec3", "vec4",
                               "error", "error", "error", "vec8",
                               "error", "error", "error", "error",
                               "error", "error", "error", "vec16"};

static void
print_register_decl(nir_register *reg, print_state *state)
{
   FILE *fp = state->fp;
   fprintf(fp, "decl_reg %s %u ", sizes[reg->num_components], reg->bit_size);
   print_register(reg, state);
   if (reg->num_array_elems != 0)
      fprintf(fp, "[%u]", reg->num_array_elems);
   fprintf(fp, "\n");
}

static void
print_ssa_def(nir_ssa_def *def, print_state *state)
{
   FILE *fp = state->fp;
   if (def->name != NULL)
      fprintf(fp, "/* %s */ ", def->name);
   fprintf(fp, "%s %u ssa_%u", sizes[def->num_components], def->bit_size,
           def->index);
}

static void
print_ssa_use(nir_ssa_def *def, print_state *state)
{
   FILE *fp = state->fp;
   if (def->name != NULL)
      fprintf(fp, "/* %s */ ", def->name);
   fprintf(fp, "ssa_%u", def->index);
}

static void print_src(const nir_src *src, print_state *state);

static void
print_reg_src(const nir_reg_src *src, print_state *state)
{
   FILE *fp = state->fp;
   print_register(src->reg, state);
   if (src->reg->num_array_elems != 0) {
      fprintf(fp, "[%u", src->base_offset);
      if (src->indirect != NULL) {
         fprintf(fp, " + ");
         print_src(src->indirect, state);
      }
      fprintf(fp, "]");
   }
}

static void
print_reg_dest(nir_reg_dest *dest, print_state *state)
{
   FILE *fp = state->fp;
   print_register(dest->reg, state);
   if (dest->reg->num_array_elems != 0) {
      fprintf(fp, "[%u", dest->base_offset);
      if (dest->indirect != NULL) {
         fprintf(fp, " + ");
         print_src(dest->indirect, state);
      }
      fprintf(fp, "]");
   }
}

static void
print_src(const nir_src *src, print_state *state)
{
   if (src->is_ssa)
      print_ssa_use(src->ssa, state);
   else
      print_reg_src(&src->reg, state);
}

static void
print_dest(nir_dest *dest, print_state *state)
{
   if (dest->is_ssa)
      print_ssa_def(&dest->ssa, state);
   else
      print_reg_dest(&dest->reg, state);
}

static const char *
comp_mask_string(unsigned num_components)
{
   return (num_components > 4) ? "abcdefghijklmnop" : "xyzw";
}

static void
print_alu_src(nir_alu_instr *instr, unsigned src, print_state *state)
{
   FILE *fp = state->fp;

   if (instr->src[src].negate)
      fprintf(fp, "-");
   if (instr->src[src].abs)
      fprintf(fp, "abs(");

   print_src(&instr->src[src].src, state);

   bool print_swizzle = false;
   nir_component_mask_t used_channels = 0;

   for (unsigned i = 0; i < NIR_MAX_VEC_COMPONENTS; i++) {
      if (!nir_alu_instr_channel_used(instr, src, i))
         continue;

      used_channels++;

      if (instr->src[src].swizzle[i] != i) {
         print_swizzle = true;
         break;
      }
   }

   unsigned live_channels = nir_src_num_components(instr->src[src].src);

   if (print_swizzle || used_channels != live_channels) {
      fprintf(fp, ".");
      for (unsigned i = 0; i < NIR_MAX_VEC_COMPONENTS; i++) {
         if (!nir_alu_instr_channel_used(instr, src, i))
            continue;

         fprintf(fp, "%c", comp_mask_string(live_channels)[instr->src[src].swizzle[i]]);
      }
   }

   if (instr->src[src].abs)
      fprintf(fp, ")");
}

static void
print_alu_dest(nir_alu_dest *dest, print_state *state)
{
   FILE *fp = state->fp;
   /* we're going to print the saturate modifier later, after the opcode */

   print_dest(&dest->dest, state);

   if (!dest->dest.is_ssa &&
       dest->write_mask != (1 << dest->dest.reg.reg->num_components) - 1) {
      unsigned live_channels = dest->dest.reg.reg->num_components;
      fprintf(fp, ".");
      for (unsigned i = 0; i < NIR_MAX_VEC_COMPONENTS; i++)
         if ((dest->write_mask >> i) & 1)
            fprintf(fp, "%c", comp_mask_string(live_channels)[i]);
   }
}

static void
print_alu_instr(nir_alu_instr *instr, print_state *state)
{
   FILE *fp = state->fp;

   print_alu_dest(&instr->dest, state);

   fprintf(fp, " = %s", nir_op_infos[instr->op].name);
   if (instr->exact)
      fprintf(fp, "!");
   if (instr->dest.saturate)
      fprintf(fp, ".sat");
   if (instr->no_signed_wrap)
      fprintf(fp, ".nsw");
   if (instr->no_unsigned_wrap)
      fprintf(fp, ".nuw");
   fprintf(fp, " ");

   for (unsigned i = 0; i < nir_op_infos[instr->op].num_inputs; i++) {
      if (i != 0)
         fprintf(fp, ", ");

      print_alu_src(instr, i, state);
   }
}

static const char *
get_var_name(nir_variable *var, print_state *state)
{
   if (state->ht == NULL)
      return var->name ? var->name : "unnamed";

   assert(state->syms);

   struct hash_entry *entry = _mesa_hash_table_search(state->ht, var);
   if (entry)
      return entry->data;

   char *name;
   if (var->name == NULL) {
      name = ralloc_asprintf(state->syms, "@%u", state->index++);
   } else {
      struct set_entry *set_entry = _mesa_set_search(state->syms, var->name);
      if (set_entry != NULL) {
         /* we have a collision with another name, append an @ + a unique
          * index */
         name = ralloc_asprintf(state->syms, "%s@%u", var->name,
                                state->index++);
      } else {
         /* Mark this one as seen */
         _mesa_set_add(state->syms, var->name);
         name = var->name;
      }
   }

   _mesa_hash_table_insert(state->ht, var, name);

   return name;
}

static void
print_constant(nir_constant *c, const struct glsl_type *type, print_state *state)
{
   FILE *fp = state->fp;
   const unsigned rows = glsl_get_vector_elements(type);
   const unsigned cols = glsl_get_matrix_columns(type);
   unsigned i;

   switch (glsl_get_base_type(type)) {
   case GLSL_TYPE_BOOL:
      /* Only float base types can be matrices. */
      assert(cols == 1);

      for (i = 0; i < rows; i++) {
         if (i > 0) fprintf(fp, ", ");
         fprintf(fp, "%s", c->values[i].b ? "true" : "false");
      }
      break;

   case GLSL_TYPE_UINT8:
   case GLSL_TYPE_INT8:
      /* Only float base types can be matrices. */
      assert(cols == 1);

      for (i = 0; i < rows; i++) {
         if (i > 0) fprintf(fp, ", ");
         fprintf(fp, "0x%02x", c->values[i].u8);
      }
      break;

   case GLSL_TYPE_UINT16:
   case GLSL_TYPE_INT16:
      /* Only float base types can be matrices. */
      assert(cols == 1);

      for (i = 0; i < rows; i++) {
         if (i > 0) fprintf(fp, ", ");
         fprintf(fp, "0x%04x", c->values[i].u16);
      }
      break;

   case GLSL_TYPE_UINT:
   case GLSL_TYPE_INT:
      /* Only float base types can be matrices. */
      assert(cols == 1);

      for (i = 0; i < rows; i++) {
         if (i > 0) fprintf(fp, ", ");
         fprintf(fp, "0x%08x", c->values[i].u32);
      }
      break;

   case GLSL_TYPE_FLOAT16:
   case GLSL_TYPE_FLOAT:
   case GLSL_TYPE_DOUBLE:
      if (cols > 1) {
         for (i = 0; i < cols; i++) {
            if (i > 0) fprintf(fp, ", ");
            print_constant(c->elements[i], glsl_get_column_type(type), state);
         }
      } else {
         switch (glsl_get_base_type(type)) {
         case GLSL_TYPE_FLOAT16:
            for (i = 0; i < rows; i++) {
               if (i > 0) fprintf(fp, ", ");
               fprintf(fp, "%f", _mesa_half_to_float(c->values[i].u16));
            }
            break;

         case GLSL_TYPE_FLOAT:
            for (i = 0; i < rows; i++) {
               if (i > 0) fprintf(fp, ", ");
               fprintf(fp, "%f", c->values[i].f32);
            }
            break;

         case GLSL_TYPE_DOUBLE:
            for (i = 0; i < rows; i++) {
               if (i > 0) fprintf(fp, ", ");
               fprintf(fp, "%f", c->values[i].f64);
            }
            break;

         default:
            unreachable("Cannot get here from the first level switch");
         }
      }
      break;

   case GLSL_TYPE_UINT64:
   case GLSL_TYPE_INT64:
      /* Only float base types can be matrices. */
      assert(cols == 1);

      for (i = 0; i < cols; i++) {
         if (i > 0) fprintf(fp, ", ");
         fprintf(fp, "0x%08" PRIx64, c->values[i].u64);
      }
      break;

   case GLSL_TYPE_STRUCT:
      for (i = 0; i < c->num_elements; i++) {
         if (i > 0) fprintf(fp, ", ");
         fprintf(fp, "{ ");
         print_constant(c->elements[i], glsl_get_struct_field(type, i), state);
         fprintf(fp, " }");
      }
      break;

   case GLSL_TYPE_ARRAY:
      for (i = 0; i < c->num_elements; i++) {
         if (i > 0) fprintf(fp, ", ");
         fprintf(fp, "{ ");
         print_constant(c->elements[i], glsl_get_array_element(type), state);
         fprintf(fp, " }");
      }
      break;

   default:
      unreachable("not reached");
   }
}

static const char *
get_variable_mode_str(nir_variable_mode mode, bool want_local_global_mode)
{
   switch (mode) {
   case nir_var_shader_in:
      return "shader_in";
   case nir_var_shader_out:
      return "shader_out";
   case nir_var_uniform:
      return "uniform";
   case nir_var_mem_ubo:
      return "ubo";
   case nir_var_system_value:
      return "system";
   case nir_var_mem_ssbo:
      return "ssbo";
   case nir_var_mem_shared:
      return "shared";
   case nir_var_mem_global:
      return "global";
   case nir_var_shader_temp:
      return want_local_global_mode ? "shader_temp" : "";
   case nir_var_function_temp:
      return want_local_global_mode ? "function_temp" : "";
   default:
      return "";
   }
}

static void
print_var_decl(nir_variable *var, print_state *state)
{
   FILE *fp = state->fp;

   fprintf(fp, "decl_var ");

   const char *const cent = (var->data.centroid) ? "centroid " : "";
   const char *const samp = (var->data.sample) ? "sample " : "";
   const char *const patch = (var->data.patch) ? "patch " : "";
   const char *const inv = (var->data.invariant) ? "invariant " : "";
   fprintf(fp, "%s%s%s%s%s %s ",
           cent, samp, patch, inv, get_variable_mode_str(var->data.mode, false),
           glsl_interp_mode_name(var->data.interpolation));

   enum gl_access_qualifier access = var->data.access;
   const char *const coher = (access & ACCESS_COHERENT) ? "coherent " : "";
   const char *const volat = (access & ACCESS_VOLATILE) ? "volatile " : "";
   const char *const restr = (access & ACCESS_RESTRICT) ? "restrict " : "";
   const char *const ronly = (access & ACCESS_NON_WRITEABLE) ? "readonly " : "";
   const char *const wonly = (access & ACCESS_NON_READABLE) ? "writeonly " : "";
   const char *const reorder = (access & ACCESS_CAN_REORDER) ? "reorderable " : "";
   fprintf(fp, "%s%s%s%s%s%s", coher, volat, restr, ronly, wonly, reorder);

   if (glsl_get_base_type(glsl_without_array(var->type)) == GLSL_TYPE_IMAGE) {
#define FORMAT_CASE(x) case x: fprintf(fp, #x " "); break
      switch (var->data.image.format) {
      FORMAT_CASE(GL_RGBA32F);
      FORMAT_CASE(GL_RGBA32UI);
      FORMAT_CASE(GL_RGBA32I);
      FORMAT_CASE(GL_R32F);
      FORMAT_CASE(GL_R32UI);
      FORMAT_CASE(GL_R32I);
      FORMAT_CASE(GL_RG32F);
      FORMAT_CASE(GL_RG32UI);
      FORMAT_CASE(GL_RG32I);
      FORMAT_CASE(GL_R8);
      FORMAT_CASE(GL_RG8);
      FORMAT_CASE(GL_RGBA8);
      FORMAT_CASE(GL_R8_SNORM);
      FORMAT_CASE(GL_RG8_SNORM);
      FORMAT_CASE(GL_RGBA8_SNORM);
      FORMAT_CASE(GL_R16);
      FORMAT_CASE(GL_RG16);
      FORMAT_CASE(GL_RGBA16);
      FORMAT_CASE(GL_R16_SNORM);
      FORMAT_CASE(GL_RG16_SNORM);
      FORMAT_CASE(GL_RGBA16_SNORM);
      FORMAT_CASE(GL_R16F);
      FORMAT_CASE(GL_RG16F);
      FORMAT_CASE(GL_RGBA16F);
      FORMAT_CASE(GL_R8UI);
      FORMAT_CASE(GL_R8I);
      FORMAT_CASE(GL_RG8UI);
      FORMAT_CASE(GL_RG8I);
      FORMAT_CASE(GL_RGBA8UI);
      FORMAT_CASE(GL_RGBA8I);
      FORMAT_CASE(GL_R16UI);
      FORMAT_CASE(GL_R16I);
      FORMAT_CASE(GL_RG16UI);
      FORMAT_CASE(GL_RG16I);
      FORMAT_CASE(GL_RGBA16UI);
      FORMAT_CASE(GL_RGBA16I);
      FORMAT_CASE(GL_R11F_G11F_B10F);
      FORMAT_CASE(GL_RGB9_E5);
      FORMAT_CASE(GL_RGB10_A2);
      FORMAT_CASE(GL_RGB10_A2UI);
      default: /* Including the normal GL_NONE */
         break;
      }
#undef FORMAT_CASE
   }

   fprintf(fp, "%s %s", glsl_get_type_name(var->type),
           get_var_name(var, state));

   if (var->data.mode == nir_var_shader_in ||
       var->data.mode == nir_var_shader_out ||
       var->data.mode == nir_var_uniform ||
       var->data.mode == nir_var_mem_ubo ||
       var->data.mode == nir_var_mem_ssbo) {
      const char *loc = NULL;
      char buf[4];

      switch (state->shader->info.stage) {
      case MESA_SHADER_VERTEX:
         if (var->data.mode == nir_var_shader_in)
            loc = gl_vert_attrib_name(var->data.location);
         else if (var->data.mode == nir_var_shader_out)
            loc = gl_varying_slot_name(var->data.location);
         break;
      case MESA_SHADER_GEOMETRY:
         if ((var->data.mode == nir_var_shader_in) ||
             (var->data.mode == nir_var_shader_out))
            loc = gl_varying_slot_name(var->data.location);
         break;
      case MESA_SHADER_FRAGMENT:
         if (var->data.mode == nir_var_shader_in)
            loc = gl_varying_slot_name(var->data.location);
         else if (var->data.mode == nir_var_shader_out)
            loc = gl_frag_result_name(var->data.location);
         break;
      case MESA_SHADER_TESS_CTRL:
      case MESA_SHADER_TESS_EVAL:
      case MESA_SHADER_COMPUTE:
      case MESA_SHADER_KERNEL:
      default:
         /* TODO */
         break;
      }

      if (!loc) {
         snprintf(buf, sizeof(buf), "%u", var->data.location);
         loc = buf;
      }

      /* For shader I/O vars that have been split to components or packed,
       * print the fractional location within the input/output.
       */
      unsigned int num_components =
         glsl_get_components(glsl_without_array(var->type));
      const char *components = NULL;
      char components_local[18] = {'.' /* the rest is 0-filled */};
      switch (var->data.mode) {
      case nir_var_shader_in:
      case nir_var_shader_out:
         if (num_components < 16 && num_components != 0) {
            const char *xyzw = comp_mask_string(num_components);
            for (int i = 0; i < num_components; i++)
               components_local[i + 1] = xyzw[i + var->data.location_frac];

            components = components_local;
         }
         break;
      default:
         break;
      }

      fprintf(fp, " (%s%s, %u, %u)%s", loc,
              components ? components : "",
              var->data.driver_location, var->data.binding,
              var->data.compact ? " compact" : "");
   }

   if (var->constant_initializer) {
      fprintf(fp, " = { ");
      print_constant(var->constant_initializer, var->type, state);
      fprintf(fp, " }");
   }

   fprintf(fp, "\n");
   print_annotation(state, var);
}

static void
print_deref_link(const nir_deref_instr *instr, bool whole_chain, print_state *state)
{
   FILE *fp = state->fp;

   if (instr->deref_type == nir_deref_type_var) {
      fprintf(fp, "%s", get_var_name(instr->var, state));
      return;
   } else if (instr->deref_type == nir_deref_type_cast) {
      fprintf(fp, "(%s *)", glsl_get_type_name(instr->type));
      print_src(&instr->parent, state);
      return;
   }

   assert(instr->parent.is_ssa);
   nir_deref_instr *parent =
      nir_instr_as_deref(instr->parent.ssa->parent_instr);

   /* Is the parent we're going to print a bare cast? */
   const bool is_parent_cast =
      whole_chain && parent->deref_type == nir_deref_type_cast;

   /* If we're not printing the whole chain, the parent we print will be a SSA
    * value that represents a pointer.  The only deref type that naturally
    * gives a pointer is a cast.
    */
   const bool is_parent_pointer =
      !whole_chain || parent->deref_type == nir_deref_type_cast;

   /* Struct derefs have a nice syntax that works on pointers, arrays derefs
    * do not.
    */
   const bool need_deref =
      is_parent_pointer && instr->deref_type != nir_deref_type_struct;

   /* Cast need extra parens and so * dereferences */
   if (is_parent_cast || need_deref)
      fprintf(fp, "(");

   if (need_deref)
      fprintf(fp, "*");

   if (whole_chain) {
      print_deref_link(parent, whole_chain, state);
   } else {
      print_src(&instr->parent, state);
   }

   if (is_parent_cast || need_deref)
      fprintf(fp, ")");

   switch (instr->deref_type) {
   case nir_deref_type_struct:
      fprintf(fp, "%s%s", is_parent_pointer ? "->" : ".",
              glsl_get_struct_elem_name(parent->type, instr->strct.index));
      break;

   case nir_deref_type_array:
   case nir_deref_type_ptr_as_array: {
      if (nir_src_is_const(instr->arr.index)) {
         fprintf(fp, "[%"PRId64"]", nir_src_as_int(instr->arr.index));
      } else {
         fprintf(fp, "[");
         print_src(&instr->arr.index, state);
         fprintf(fp, "]");
      }
      break;
   }

   case nir_deref_type_array_wildcard:
      fprintf(fp, "[*]");
      break;

   default:
      unreachable("Invalid deref instruction type");
   }
}

static void
print_deref_instr(nir_deref_instr *instr, print_state *state)
{
   FILE *fp = state->fp;

   print_dest(&instr->dest, state);

   switch (instr->deref_type) {
   case nir_deref_type_var:
      fprintf(fp, " = deref_var ");
      break;
   case nir_deref_type_array:
   case nir_deref_type_array_wildcard:
      fprintf(fp, " = deref_array ");
      break;
   case nir_deref_type_struct:
      fprintf(fp, " = deref_struct ");
      break;
   case nir_deref_type_cast:
      fprintf(fp, " = deref_cast ");
      break;
   case nir_deref_type_ptr_as_array:
      fprintf(fp, " = deref_ptr_as_array ");
      break;
   default:
      unreachable("Invalid deref instruction type");
   }

   /* Only casts naturally return a pointer type */
   if (instr->deref_type != nir_deref_type_cast)
      fprintf(fp, "&");

   print_deref_link(instr, false, state);

   fprintf(fp, " (%s %s) ",
           get_variable_mode_str(instr->mode, true),
           glsl_get_type_name(instr->type));

   if (instr->deref_type != nir_deref_type_var &&
       instr->deref_type != nir_deref_type_cast) {
      /* Print the entire chain as a comment */
      fprintf(fp, "/* &");
      print_deref_link(instr, true, state);
      fprintf(fp, " */");
   }

   if (instr->deref_type == nir_deref_type_cast) {
      fprintf(fp, " /* ptr_stride=%u */", instr->cast.ptr_stride);
   }
}

static const char *
vulkan_descriptor_type_name(VkDescriptorType type)
{
   switch (type) {
   case VK_DESCRIPTOR_TYPE_SAMPLER: return "sampler";
   case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return "texture+sampler";
   case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: return "texture";
   case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: return "image";
   case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: return "texture-buffer";
   case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return "image-buffer";
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: return "UBO";
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: return "SSBO";
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: return "UBO";
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: return "SSBO";
   case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: return "input-att";
   case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT: return "inline-UBO";
   default: return "unknown";
   }
}

static void
print_intrinsic_instr(nir_intrinsic_instr *instr, print_state *state)
{
   const nir_intrinsic_info *info = &nir_intrinsic_infos[instr->intrinsic];
   unsigned num_srcs = info->num_srcs;
   FILE *fp = state->fp;

   if (info->has_dest) {
      print_dest(&instr->dest, state);
      fprintf(fp, " = ");
   }

   fprintf(fp, "intrinsic %s (", info->name);

   for (unsigned i = 0; i < num_srcs; i++) {
      if (i != 0)
         fprintf(fp, ", ");

      print_src(&instr->src[i], state);
   }

   fprintf(fp, ") (");

   for (unsigned i = 0; i < info->num_indices; i++) {
      if (i != 0)
         fprintf(fp, ", ");

      fprintf(fp, "%d", instr->const_index[i]);
   }

   fprintf(fp, ")");

   static const char *index_name[NIR_INTRINSIC_NUM_INDEX_FLAGS] = {
      [NIR_INTRINSIC_BASE] = "base",
      [NIR_INTRINSIC_WRMASK] = "wrmask",
      [NIR_INTRINSIC_STREAM_ID] = "stream-id",
      [NIR_INTRINSIC_UCP_ID] = "ucp-id",
      [NIR_INTRINSIC_RANGE] = "range",
      [NIR_INTRINSIC_DESC_SET] = "desc-set",
      [NIR_INTRINSIC_BINDING] = "binding",
      [NIR_INTRINSIC_COMPONENT] = "component",
      [NIR_INTRINSIC_INTERP_MODE] = "interp_mode",
      [NIR_INTRINSIC_REDUCTION_OP] = "reduction_op",
      [NIR_INTRINSIC_CLUSTER_SIZE] = "cluster_size",
      [NIR_INTRINSIC_PARAM_IDX] = "param_idx",
      [NIR_INTRINSIC_IMAGE_DIM] = "image_dim",
      [NIR_INTRINSIC_IMAGE_ARRAY] = "image_array",
      [NIR_INTRINSIC_ACCESS] = "access",
      [NIR_INTRINSIC_SRC_ACCESS] = "src-access",
      [NIR_INTRINSIC_DST_ACCESS] = "dst-access",
      [NIR_INTRINSIC_FORMAT] = "format",
      [NIR_INTRINSIC_ALIGN_MUL] = "align_mul",
      [NIR_INTRINSIC_ALIGN_OFFSET] = "align_offset",
      [NIR_INTRINSIC_DESC_TYPE] = "desc_type",
      [NIR_INTRINSIC_TYPE] = "type",
      [NIR_INTRINSIC_SWIZZLE_MASK] = "swizzle_mask",
      [NIR_INTRINSIC_DRIVER_LOCATION] = "driver_location",
      [NIR_INTRINSIC_MEMORY_SEMANTICS] = "mem_semantics",
      [NIR_INTRINSIC_MEMORY_MODES] = "mem_modes",
      [NIR_INTRINSIC_MEMORY_SCOPE] = "mem_scope",
   };
   for (unsigned idx = 1; idx < NIR_INTRINSIC_NUM_INDEX_FLAGS; idx++) {
      if (!info->index_map[idx])
         continue;
      fprintf(fp, " /*");
      switch (idx) {
      case NIR_INTRINSIC_WRMASK: {
         /* special case wrmask to show it as a writemask.. */
         unsigned wrmask = nir_intrinsic_write_mask(instr);
         fprintf(fp, " wrmask=");
         for (unsigned i = 0; i < instr->num_components; i++)
            if ((wrmask >> i) & 1)
               fprintf(fp, "%c", comp_mask_string(instr->num_components)[i]);
         break;
      }

      case NIR_INTRINSIC_REDUCTION_OP: {
         nir_op reduction_op = nir_intrinsic_reduction_op(instr);
         fprintf(fp, " reduction_op=%s", nir_op_infos[reduction_op].name);
         break;
      }

      case NIR_INTRINSIC_IMAGE_DIM: {
         static const char *dim_name[] = {
            [GLSL_SAMPLER_DIM_1D] = "1D",
            [GLSL_SAMPLER_DIM_2D] = "2D",
            [GLSL_SAMPLER_DIM_3D] = "3D",
            [GLSL_SAMPLER_DIM_CUBE] = "Cube",
            [GLSL_SAMPLER_DIM_RECT] = "Rect",
            [GLSL_SAMPLER_DIM_BUF] = "Buf",
            [GLSL_SAMPLER_DIM_MS] = "2D-MSAA",
            [GLSL_SAMPLER_DIM_SUBPASS] = "Subpass",
            [GLSL_SAMPLER_DIM_SUBPASS_MS] = "Subpass-MSAA",
         };
         enum glsl_sampler_dim dim = nir_intrinsic_image_dim(instr);
         assert(dim < ARRAY_SIZE(dim_name) && dim_name[dim]);
         fprintf(fp, " image_dim=%s", dim_name[dim]);
         break;
      }

      case NIR_INTRINSIC_IMAGE_ARRAY: {
         bool array = nir_intrinsic_image_array(instr);
         fprintf(fp, " image_array=%s", array ? "true" : "false");
         break;
      }

      case NIR_INTRINSIC_DESC_TYPE: {
         VkDescriptorType desc_type = nir_intrinsic_desc_type(instr);
         fprintf(fp, " desc_type=%s", vulkan_descriptor_type_name(desc_type));
         break;
      }

      case NIR_INTRINSIC_TYPE: {
         nir_alu_type type = nir_intrinsic_type(instr);
         unsigned size = nir_alu_type_get_type_size(type);
         const char *name;
         switch (nir_alu_type_get_base_type(type)) {
         case nir_type_int: name = "int"; break;
         case nir_type_uint: name = "uint"; break;
         case nir_type_bool: name = "bool"; break;
         case nir_type_float: name = "float"; break;
         default: name = "invalid";
         }
         if (size)
            fprintf(fp, " type=%s%u", name, size);
         else
            fprintf(fp, " type=%s", name);
         break;
      }

      case NIR_INTRINSIC_SWIZZLE_MASK: {
         fprintf(fp, " swizzle_mask=");
         unsigned mask = nir_intrinsic_swizzle_mask(instr);
         if (instr->intrinsic == nir_intrinsic_quad_swizzle_amd) {
            for (unsigned i = 0; i < 4; i++)
               fprintf(fp, "%d", (mask >> (i * 2) & 3));
         } else if (instr->intrinsic == nir_intrinsic_masked_swizzle_amd) {
            fprintf(fp, "((id & %d) | %d) ^ %d", mask & 0x1F,
                                                (mask >> 5) & 0x1F,
                                                (mask >> 10) & 0x1F);
         } else {
            fprintf(fp, "%d", mask);
         }
         break;
      }

      case NIR_INTRINSIC_MEMORY_SEMANTICS: {
         nir_memory_semantics semantics = nir_intrinsic_memory_semantics(instr);
         fprintf(fp, " mem_semantics=");
         switch (semantics & (NIR_MEMORY_ACQUIRE | NIR_MEMORY_RELEASE)) {
         case 0:                  fprintf(fp, "NONE");    break;
         case NIR_MEMORY_ACQUIRE: fprintf(fp, "ACQ");     break;
         case NIR_MEMORY_RELEASE: fprintf(fp, "REL");     break;
         default:                 fprintf(fp, "ACQ|REL"); break;
         }
         if (semantics & (NIR_MEMORY_MAKE_AVAILABLE)) fprintf(fp, "|AVAILABLE");
         if (semantics & (NIR_MEMORY_MAKE_VISIBLE))   fprintf(fp, "|VISIBLE");
         break;
      }

      case NIR_INTRINSIC_MEMORY_MODES: {
         fprintf(fp, " mem_modes=");
         unsigned int modes = nir_intrinsic_memory_modes(instr);
         while (modes) {
            nir_variable_mode m = u_bit_scan(&modes);
            fprintf(fp, "%s%s", get_variable_mode_str(1 << m, true), modes ? "|" : "");
         }
         break;
      }

      case NIR_INTRINSIC_MEMORY_SCOPE: {
         fprintf(fp, " mem_scope=");
         switch (nir_intrinsic_memory_scope(instr)) {
         case NIR_SCOPE_DEVICE:       fprintf(fp, "DEVICE");       break;
         case NIR_SCOPE_QUEUE_FAMILY: fprintf(fp, "QUEUE_FAMILY"); break;
         case NIR_SCOPE_WORKGROUP:    fprintf(fp, "WORKGROUP");    break;
         case NIR_SCOPE_SUBGROUP:     fprintf(fp, "SUBGROUP");     break;
         case NIR_SCOPE_INVOCATION:   fprintf(fp, "INVOCATION");   break;
         }
         break;
      }

      default: {
         unsigned off = info->index_map[idx] - 1;
         assert(index_name[idx]);  /* forgot to update index_name table? */
         fprintf(fp, " %s=%d", index_name[idx], instr->const_index[off]);
         break;
      }
      }
      fprintf(fp, " */");
   }

   if (!state->shader)
      return;

   struct exec_list *var_list = NULL;

   switch (instr->intrinsic) {
   case nir_intrinsic_load_uniform:
      var_list = &state->shader->uniforms;
      break;
   case nir_intrinsic_load_input:
   case nir_intrinsic_load_interpolated_input:
   case nir_intrinsic_load_per_vertex_input:
      var_list = &state->shader->inputs;
      break;
   case nir_intrinsic_load_output:
   case nir_intrinsic_store_output:
   case nir_intrinsic_store_per_vertex_output:
      var_list = &state->shader->outputs;
      break;
   default:
      return;
   }

   nir_foreach_variable(var, var_list) {
      if ((var->data.driver_location == nir_intrinsic_base(instr)) &&
          (instr->intrinsic == nir_intrinsic_load_uniform ||
           (nir_intrinsic_component(instr) >= var->data.location_frac  &&
            nir_intrinsic_component(instr) <
            (var->data.location_frac + glsl_get_components(var->type)))) &&
           var->name) {
         fprintf(fp, "\t/* %s */", var->name);
         break;
      }
   }
}

static void
print_tex_instr(nir_tex_instr *instr, print_state *state)
{
   FILE *fp = state->fp;

   print_dest(&instr->dest, state);

   fprintf(fp, " = ");

   switch (instr->op) {
   case nir_texop_tex:
      fprintf(fp, "tex ");
      break;
   case nir_texop_txb:
      fprintf(fp, "txb ");
      break;
   case nir_texop_txl:
      fprintf(fp, "txl ");
      break;
   case nir_texop_txd:
      fprintf(fp, "txd ");
      break;
   case nir_texop_txf:
      fprintf(fp, "txf ");
      break;
   case nir_texop_txf_ms:
      fprintf(fp, "txf_ms ");
      break;
   case nir_texop_txf_ms_fb:
      fprintf(fp, "txf_ms_fb ");
      break;
   case nir_texop_txf_ms_mcs:
      fprintf(fp, "txf_ms_mcs ");
      break;
   case nir_texop_txs:
      fprintf(fp, "txs ");
      break;
   case nir_texop_lod:
      fprintf(fp, "lod ");
      break;
   case nir_texop_tg4:
      fprintf(fp, "tg4 ");
      break;
   case nir_texop_query_levels:
      fprintf(fp, "query_levels ");
      break;
   case nir_texop_texture_samples:
      fprintf(fp, "texture_samples ");
      break;
   case nir_texop_samples_identical:
      fprintf(fp, "samples_identical ");
      break;
   case nir_texop_tex_prefetch:
      fprintf(fp, "tex (pre-dispatchable) ");
      break;
   default:
      unreachable("Invalid texture operation");
      break;
   }

   bool has_texture_deref = false, has_sampler_deref = false;
   for (unsigned i = 0; i < instr->num_srcs; i++) {
      if (i > 0) {
         fprintf(fp, ", ");
      }

      print_src(&instr->src[i].src, state);
      fprintf(fp, " ");

      switch(instr->src[i].src_type) {
      case nir_tex_src_coord:
         fprintf(fp, "(coord)");
         break;
      case nir_tex_src_projector:
         fprintf(fp, "(projector)");
         break;
      case nir_tex_src_comparator:
         fprintf(fp, "(comparator)");
         break;
      case nir_tex_src_offset:
         fprintf(fp, "(offset)");
         break;
      case nir_tex_src_bias:
         fprintf(fp, "(bias)");
         break;
      case nir_tex_src_lod:
         fprintf(fp, "(lod)");
         break;
      case nir_tex_src_min_lod:
         fprintf(fp, "(min_lod)");
         break;
      case nir_tex_src_ms_index:
         fprintf(fp, "(ms_index)");
         break;
      case nir_tex_src_ms_mcs:
         fprintf(fp, "(ms_mcs)");
         break;
      case nir_tex_src_ddx:
         fprintf(fp, "(ddx)");
         break;
      case nir_tex_src_ddy:
         fprintf(fp, "(ddy)");
         break;
      case nir_tex_src_texture_deref:
         has_texture_deref = true;
         fprintf(fp, "(texture_deref)");
         break;
      case nir_tex_src_sampler_deref:
         has_sampler_deref = true;
         fprintf(fp, "(sampler_deref)");
         break;
      case nir_tex_src_texture_offset:
         fprintf(fp, "(texture_offset)");
         break;
      case nir_tex_src_sampler_offset:
         fprintf(fp, "(sampler_offset)");
         break;
      case nir_tex_src_texture_handle:
         fprintf(fp, "(texture_handle)");
         break;
      case nir_tex_src_sampler_handle:
         fprintf(fp, "(sampler_handle)");
         break;
      case nir_tex_src_plane:
         fprintf(fp, "(plane)");
         break;

      default:
         unreachable("Invalid texture source type");
         break;
      }
   }

   if (instr->op == nir_texop_tg4) {
      fprintf(fp, ", %u (gather_component)", instr->component);
   }

   if (nir_tex_instr_has_explicit_tg4_offsets(instr)) {
      fprintf(fp, ", { (%i, %i)", instr->tg4_offsets[0][0], instr->tg4_offsets[0][1]);
      for (unsigned i = 1; i < 4; ++i)
         fprintf(fp, ", (%i, %i)", instr->tg4_offsets[i][0],
                 instr->tg4_offsets[i][1]);
      fprintf(fp, " } (offsets)");
   }

   if (instr->op != nir_texop_txf_ms_fb) {
      if (!has_texture_deref) {
         fprintf(fp, ", %u (texture)", instr->texture_index);
      }

      if (!has_sampler_deref) {
         fprintf(fp, ", %u (sampler)", instr->sampler_index);
      }
   }

   if (instr->texture_non_uniform) {
      fprintf(fp, ", texture non-uniform");
   }

   if (instr->sampler_non_uniform) {
      fprintf(fp, ", sampler non-uniform");
   }
}

static void
print_call_instr(nir_call_instr *instr, print_state *state)
{
   FILE *fp = state->fp;

   fprintf(fp, "call %s ", instr->callee->name);

   for (unsigned i = 0; i < instr->num_params; i++) {
      if (i != 0)
         fprintf(fp, ", ");

      print_src(&instr->params[i], state);
   }
}

static void
print_load_const_instr(nir_load_const_instr *instr, print_state *state)
{
   FILE *fp = state->fp;

   print_ssa_def(&instr->def, state);

   fprintf(fp, " = load_const (");

   for (unsigned i = 0; i < instr->def.num_components; i++) {
      if (i != 0)
         fprintf(fp, ", ");

      /*
       * we don't really know the type of the constant (if it will be used as a
       * float or an int), so just print the raw constant in hex for fidelity
       * and then print the float in a comment for readability.
       */

      switch (instr->def.bit_size) {
      case 64:
         fprintf(fp, "0x%16" PRIx64 " /* %f */", instr->value[i].u64,
                 instr->value[i].f64);
         break;
      case 32:
         fprintf(fp, "0x%08x /* %f */", instr->value[i].u32, instr->value[i].f32);
         break;
      case 16:
         fprintf(fp, "0x%04x /* %f */", instr->value[i].u16,
                 _mesa_half_to_float(instr->value[i].u16));
         break;
      case 8:
         fprintf(fp, "0x%02x", instr->value[i].u8);
         break;
      case 1:
         fprintf(fp, "%s", instr->value[i].b ? "true" : "false");
         break;
      }
   }

   fprintf(fp, ")");
}

static void
print_jump_instr(nir_jump_instr *instr, print_state *state)
{
   FILE *fp = state->fp;

   switch (instr->type) {
   case nir_jump_break:
      fprintf(fp, "break");
      break;

   case nir_jump_continue:
      fprintf(fp, "continue");
      break;

   case nir_jump_return:
      fprintf(fp, "return");
      break;
   }
}

static void
print_ssa_undef_instr(nir_ssa_undef_instr* instr, print_state *state)
{
   FILE *fp = state->fp;
   print_ssa_def(&instr->def, state);
   fprintf(fp, " = undefined");
}

static void
print_phi_instr(nir_phi_instr *instr, print_state *state)
{
   FILE *fp = state->fp;
   print_dest(&instr->dest, state);
   fprintf(fp, " = phi ");
   nir_foreach_phi_src(src, instr) {
      if (&src->node != exec_list_get_head(&instr->srcs))
         fprintf(fp, ", ");

      fprintf(fp, "block_%u: ", src->pred->index);
      print_src(&src->src, state);
   }
}

static void
print_parallel_copy_instr(nir_parallel_copy_instr *instr, print_state *state)
{
   FILE *fp = state->fp;
   nir_foreach_parallel_copy_entry(entry, instr) {
      if (&entry->node != exec_list_get_head(&instr->entries))
         fprintf(fp, "; ");

      print_dest(&entry->dest, state);
      fprintf(fp, " = ");
      print_src(&entry->src, state);
   }
}

static void
print_instr(const nir_instr *instr, print_state *state, unsigned tabs)
{
   FILE *fp = state->fp;
   print_tabs(tabs, fp);

   switch (instr->type) {
   case nir_instr_type_alu:
      print_alu_instr(nir_instr_as_alu(instr), state);
      break;

   case nir_instr_type_deref:
      print_deref_instr(nir_instr_as_deref(instr), state);
      break;

   case nir_instr_type_call:
      print_call_instr(nir_instr_as_call(instr), state);
      break;

   case nir_instr_type_intrinsic:
      print_intrinsic_instr(nir_instr_as_intrinsic(instr), state);
      break;

   case nir_instr_type_tex:
      print_tex_instr(nir_instr_as_tex(instr), state);
      break;

   case nir_instr_type_load_const:
      print_load_const_instr(nir_instr_as_load_const(instr), state);
      break;

   case nir_instr_type_jump:
      print_jump_instr(nir_instr_as_jump(instr), state);
      break;

   case nir_instr_type_ssa_undef:
      print_ssa_undef_instr(nir_instr_as_ssa_undef(instr), state);
      break;

   case nir_instr_type_phi:
      print_phi_instr(nir_instr_as_phi(instr), state);
      break;

   case nir_instr_type_parallel_copy:
      print_parallel_copy_instr(nir_instr_as_parallel_copy(instr), state);
      break;

   default:
      unreachable("Invalid instruction type");
      break;
   }
}

static int
compare_block_index(const void *p1, const void *p2)
{
   const nir_block *block1 = *((const nir_block **) p1);
   const nir_block *block2 = *((const nir_block **) p2);

   return (int) block1->index - (int) block2->index;
}

static void print_cf_node(nir_cf_node *node, print_state *state,
                          unsigned tabs);

static void
print_block(nir_block *block, print_state *state, unsigned tabs)
{
   FILE *fp = state->fp;

   print_tabs(tabs, fp);
   fprintf(fp, "block block_%u:\n", block->index);

   /* sort the predecessors by index so we consistently print the same thing */

   nir_block **preds =
      malloc(block->predecessors->entries * sizeof(nir_block *));

   unsigned i = 0;
   set_foreach(block->predecessors, entry) {
      preds[i++] = (nir_block *) entry->key;
   }

   qsort(preds, block->predecessors->entries, sizeof(nir_block *),
         compare_block_index);

   print_tabs(tabs, fp);
   fprintf(fp, "/* preds: ");
   for (unsigned i = 0; i < block->predecessors->entries; i++) {
      fprintf(fp, "block_%u ", preds[i]->index);
   }
   fprintf(fp, "*/\n");

   free(preds);

   nir_foreach_instr(instr, block) {
      print_instr(instr, state, tabs);
      fprintf(fp, "\n");
      print_annotation(state, instr);
   }

   print_tabs(tabs, fp);
   fprintf(fp, "/* succs: ");
   for (unsigned i = 0; i < 2; i++)
      if (block->successors[i]) {
         fprintf(fp, "block_%u ", block->successors[i]->index);
      }
   fprintf(fp, "*/\n");
}

static void
print_if(nir_if *if_stmt, print_state *state, unsigned tabs)
{
   FILE *fp = state->fp;

   print_tabs(tabs, fp);
   fprintf(fp, "if ");
   print_src(&if_stmt->condition, state);
   fprintf(fp, " {\n");
   foreach_list_typed(nir_cf_node, node, node, &if_stmt->then_list) {
      print_cf_node(node, state, tabs + 1);
   }
   print_tabs(tabs, fp);
   fprintf(fp, "} else {\n");
   foreach_list_typed(nir_cf_node, node, node, &if_stmt->else_list) {
      print_cf_node(node, state, tabs + 1);
   }
   print_tabs(tabs, fp);
   fprintf(fp, "}\n");
}

static void
print_loop(nir_loop *loop, print_state *state, unsigned tabs)
{
   FILE *fp = state->fp;

   print_tabs(tabs, fp);
   fprintf(fp, "loop {\n");
   foreach_list_typed(nir_cf_node, node, node, &loop->body) {
      print_cf_node(node, state, tabs + 1);
   }
   print_tabs(tabs, fp);
   fprintf(fp, "}\n");
}

static void
print_cf_node(nir_cf_node *node, print_state *state, unsigned int tabs)
{
   switch (node->type) {
   case nir_cf_node_block:
      print_block(nir_cf_node_as_block(node), state, tabs);
      break;

   case nir_cf_node_if:
      print_if(nir_cf_node_as_if(node), state, tabs);
      break;

   case nir_cf_node_loop:
      print_loop(nir_cf_node_as_loop(node), state, tabs);
      break;

   default:
      unreachable("Invalid CFG node type");
   }
}

static void
print_function_impl(nir_function_impl *impl, print_state *state)
{
   FILE *fp = state->fp;

   fprintf(fp, "\nimpl %s ", impl->function->name);

   fprintf(fp, "{\n");

   nir_foreach_variable(var, &impl->locals) {
      fprintf(fp, "\t");
      print_var_decl(var, state);
   }

   foreach_list_typed(nir_register, reg, node, &impl->registers) {
      fprintf(fp, "\t");
      print_register_decl(reg, state);
   }

   nir_index_blocks(impl);

   foreach_list_typed(nir_cf_node, node, node, &impl->body) {
      print_cf_node(node, state, 1);
   }

   fprintf(fp, "\tblock block_%u:\n}\n\n", impl->end_block->index);
}

static void
print_function(nir_function *function, print_state *state)
{
   FILE *fp = state->fp;

   fprintf(fp, "decl_function %s (%d params)", function->name,
           function->num_params);

   fprintf(fp, "\n");

   if (function->impl != NULL) {
      print_function_impl(function->impl, state);
      return;
   }
}

static void
init_print_state(print_state *state, nir_shader *shader, FILE *fp)
{
   state->fp = fp;
   state->shader = shader;
   state->ht = _mesa_pointer_hash_table_create(NULL);
   state->syms = _mesa_set_create(NULL, _mesa_key_hash_string,
                                  _mesa_key_string_equal);
   state->index = 0;
}

static void
destroy_print_state(print_state *state)
{
   _mesa_hash_table_destroy(state->ht, NULL);
   _mesa_set_destroy(state->syms, NULL);
}

void
nir_print_shader_annotated(nir_shader *shader, FILE *fp,
                           struct hash_table *annotations)
{
   print_state state;
   init_print_state(&state, shader, fp);

   state.annotations = annotations;

   fprintf(fp, "shader: %s\n", gl_shader_stage_name(shader->info.stage));

   if (shader->info.name)
      fprintf(fp, "name: %s\n", shader->info.name);

   if (shader->info.label)
      fprintf(fp, "label: %s\n", shader->info.label);

   if (gl_shader_stage_is_compute(shader->info.stage)) {
      fprintf(fp, "local-size: %u, %u, %u%s\n",
              shader->info.cs.local_size[0],
              shader->info.cs.local_size[1],
              shader->info.cs.local_size[2],
              shader->info.cs.local_size_variable ? " (variable)" : "");
      fprintf(fp, "shared-size: %u\n", shader->info.cs.shared_size);
   }

   fprintf(fp, "inputs: %u\n", shader->num_inputs);
   fprintf(fp, "outputs: %u\n", shader->num_outputs);
   fprintf(fp, "uniforms: %u\n", shader->num_uniforms);
   fprintf(fp, "shared: %u\n", shader->num_shared);
   if (shader->scratch_size)
      fprintf(fp, "scratch: %u\n", shader->scratch_size);

   nir_foreach_variable(var, &shader->uniforms) {
      print_var_decl(var, &state);
   }

   nir_foreach_variable(var, &shader->inputs) {
      print_var_decl(var, &state);
   }

   nir_foreach_variable(var, &shader->outputs) {
      print_var_decl(var, &state);
   }

   nir_foreach_variable(var, &shader->shared) {
      print_var_decl(var, &state);
   }

   nir_foreach_variable(var, &shader->globals) {
      print_var_decl(var, &state);
   }

   nir_foreach_variable(var, &shader->system_values) {
      print_var_decl(var, &state);
   }

   foreach_list_typed(nir_function, func, node, &shader->functions) {
      print_function(func, &state);
   }

   destroy_print_state(&state);
}

void
nir_print_shader(nir_shader *shader, FILE *fp)
{
   nir_print_shader_annotated(shader, fp, NULL);
   fflush(fp);
}

void
nir_print_instr(const nir_instr *instr, FILE *fp)
{
   print_state state = {
      .fp = fp,
   };
   print_instr(instr, &state, 0);

}

void
nir_print_deref(const nir_deref_instr *deref, FILE *fp)
{
   print_state state = {
      .fp = fp,
   };
   print_deref_link(deref, true, &state);
}
