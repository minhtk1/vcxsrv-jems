/**************************************************************************
 * 
 * Copyright 2007 VMware, Inc.
 * All Rights Reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 **************************************************************************/
 /*
  * Authors:
  *   Keith Whitwell <keithw@vmware.com>
  *   Brian Paul
  */


#include "main/errors.h"
#include "main/imports.h"
#include "main/hash.h"
#include "main/mtypes.h"
#include "program/prog_parameter.h"
#include "program/prog_print.h"
#include "program/prog_to_nir.h"
#include "program/programopt.h"

#include "compiler/nir/nir.h"
#include "draw/draw_context.h"

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_shader_tokens.h"
#include "draw/draw_context.h"
#include "tgsi/tgsi_dump.h"
#include "tgsi/tgsi_emulate.h"
#include "tgsi/tgsi_parse.h"
#include "tgsi/tgsi_ureg.h"

#include "st_debug.h"
#include "st_cb_bitmap.h"
#include "st_cb_drawpixels.h"
#include "st_context.h"
#include "st_tgsi_lower_depth_clamp.h"
#include "st_tgsi_lower_yuv.h"
#include "st_program.h"
#include "st_mesa_to_tgsi.h"
#include "st_atifs_to_tgsi.h"
#include "st_nir.h"
#include "st_shader_cache.h"
#include "st_util.h"
#include "cso_cache/cso_context.h"



static void
set_affected_state_flags(uint64_t *states,
                         struct gl_program *prog,
                         uint64_t new_constants,
                         uint64_t new_sampler_views,
                         uint64_t new_samplers,
                         uint64_t new_images,
                         uint64_t new_ubos,
                         uint64_t new_ssbos,
                         uint64_t new_atomics)
{
   if (prog->Parameters->NumParameters)
      *states |= new_constants;

   if (prog->info.num_textures)
      *states |= new_sampler_views | new_samplers;

   if (prog->info.num_images)
      *states |= new_images;

   if (prog->info.num_ubos)
      *states |= new_ubos;

   if (prog->info.num_ssbos)
      *states |= new_ssbos;

   if (prog->info.num_abos)
      *states |= new_atomics;
}

/**
 * This determines which states will be updated when the shader is bound.
 */
void
st_set_prog_affected_state_flags(struct gl_program *prog)
{
   uint64_t *states;

   switch (prog->info.stage) {
   case MESA_SHADER_VERTEX:
      states = &((struct st_program*)prog)->affected_states;

      *states = ST_NEW_VS_STATE |
                ST_NEW_RASTERIZER |
                ST_NEW_VERTEX_ARRAYS;

      set_affected_state_flags(states, prog,
                               ST_NEW_VS_CONSTANTS,
                               ST_NEW_VS_SAMPLER_VIEWS,
                               ST_NEW_VS_SAMPLERS,
                               ST_NEW_VS_IMAGES,
                               ST_NEW_VS_UBOS,
                               ST_NEW_VS_SSBOS,
                               ST_NEW_VS_ATOMICS);
      break;

   case MESA_SHADER_TESS_CTRL:
      states = &(st_program(prog))->affected_states;

      *states = ST_NEW_TCS_STATE;

      set_affected_state_flags(states, prog,
                               ST_NEW_TCS_CONSTANTS,
                               ST_NEW_TCS_SAMPLER_VIEWS,
                               ST_NEW_TCS_SAMPLERS,
                               ST_NEW_TCS_IMAGES,
                               ST_NEW_TCS_UBOS,
                               ST_NEW_TCS_SSBOS,
                               ST_NEW_TCS_ATOMICS);
      break;

   case MESA_SHADER_TESS_EVAL:
      states = &(st_program(prog))->affected_states;

      *states = ST_NEW_TES_STATE |
                ST_NEW_RASTERIZER;

      set_affected_state_flags(states, prog,
                               ST_NEW_TES_CONSTANTS,
                               ST_NEW_TES_SAMPLER_VIEWS,
                               ST_NEW_TES_SAMPLERS,
                               ST_NEW_TES_IMAGES,
                               ST_NEW_TES_UBOS,
                               ST_NEW_TES_SSBOS,
                               ST_NEW_TES_ATOMICS);
      break;

   case MESA_SHADER_GEOMETRY:
      states = &(st_program(prog))->affected_states;

      *states = ST_NEW_GS_STATE |
                ST_NEW_RASTERIZER;

      set_affected_state_flags(states, prog,
                               ST_NEW_GS_CONSTANTS,
                               ST_NEW_GS_SAMPLER_VIEWS,
                               ST_NEW_GS_SAMPLERS,
                               ST_NEW_GS_IMAGES,
                               ST_NEW_GS_UBOS,
                               ST_NEW_GS_SSBOS,
                               ST_NEW_GS_ATOMICS);
      break;

   case MESA_SHADER_FRAGMENT:
      states = &((struct st_program*)prog)->affected_states;

      /* gl_FragCoord and glDrawPixels always use constants. */
      *states = ST_NEW_FS_STATE |
                ST_NEW_SAMPLE_SHADING |
                ST_NEW_FS_CONSTANTS;

      set_affected_state_flags(states, prog,
                               ST_NEW_FS_CONSTANTS,
                               ST_NEW_FS_SAMPLER_VIEWS,
                               ST_NEW_FS_SAMPLERS,
                               ST_NEW_FS_IMAGES,
                               ST_NEW_FS_UBOS,
                               ST_NEW_FS_SSBOS,
                               ST_NEW_FS_ATOMICS);
      break;

   case MESA_SHADER_COMPUTE:
      states = &((struct st_program*)prog)->affected_states;

      *states = ST_NEW_CS_STATE;

      set_affected_state_flags(states, prog,
                               ST_NEW_CS_CONSTANTS,
                               ST_NEW_CS_SAMPLER_VIEWS,
                               ST_NEW_CS_SAMPLERS,
                               ST_NEW_CS_IMAGES,
                               ST_NEW_CS_UBOS,
                               ST_NEW_CS_SSBOS,
                               ST_NEW_CS_ATOMICS);
      break;

   default:
      unreachable("unhandled shader stage");
   }
}


/**
 * Delete a shader variant.  Note the caller must unlink the variant from
 * the linked list.
 */
static void
delete_variant(struct st_context *st, struct st_variant *v, GLenum target)
{
   if (v->driver_shader) {
      if (target == GL_VERTEX_PROGRAM_ARB &&
          ((struct st_common_variant*)v)->key.is_draw_shader) {
         /* Draw shader. */
         draw_delete_vertex_shader(st->draw, v->driver_shader);
      } else if (st->has_shareable_shaders || v->st == st) {
         /* The shader's context matches the calling context, or we
          * don't care.
          */
         switch (target) {
         case GL_VERTEX_PROGRAM_ARB:
            cso_delete_vertex_shader(st->cso_context, v->driver_shader);
            break;
         case GL_TESS_CONTROL_PROGRAM_NV:
            cso_delete_tessctrl_shader(st->cso_context, v->driver_shader);
            break;
         case GL_TESS_EVALUATION_PROGRAM_NV:
            cso_delete_tesseval_shader(st->cso_context, v->driver_shader);
            break;
         case GL_GEOMETRY_PROGRAM_NV:
            cso_delete_geometry_shader(st->cso_context, v->driver_shader);
            break;
         case GL_FRAGMENT_PROGRAM_ARB:
            cso_delete_fragment_shader(st->cso_context, v->driver_shader);
            break;
         case GL_COMPUTE_PROGRAM_NV:
            cso_delete_compute_shader(st->cso_context, v->driver_shader);
            break;
         default:
            unreachable("bad shader type in delete_basic_variant");
         }
      } else {
         /* We can't delete a shader with a context different from the one
          * that created it.  Add it to the creating context's zombie list.
          */
         enum pipe_shader_type type =
            pipe_shader_type_from_mesa(_mesa_program_enum_to_shader_stage(target));

         st_save_zombie_shader(v->st, type, v->driver_shader);
      }
   }

   free(v);
}


/**
 * Free all basic program variants.
 */
void
st_release_variants(struct st_context *st, struct st_program *p)
{
   struct st_variant *v;

   for (v = p->variants; v; ) {
      struct st_variant *next = v->next;
      delete_variant(st, v, p->Base.Target);
      v = next;
   }

   p->variants = NULL;

   if (p->state.tokens) {
      ureg_free_tokens(p->state.tokens);
      p->state.tokens = NULL;
   }

   /* Note: Any setup of ->ir.nir that has had pipe->create_*_state called on
    * it has resulted in the driver taking ownership of the NIR.  Those
    * callers should be NULLing out the nir field in any pipe_shader_state
    * that might have this called in order to indicate that.
    *
    * GLSL IR and ARB programs will have set gl_program->nir to the same
    * shader as ir->ir.nir, so it will be freed by _mesa_delete_program().
    */
}

void
st_finalize_nir_before_variants(struct nir_shader *nir)
{
   NIR_PASS_V(nir, nir_opt_access);

   NIR_PASS_V(nir, nir_split_var_copies);
   NIR_PASS_V(nir, nir_lower_var_copies);
   if (nir->options->lower_all_io_to_temps ||
       nir->options->lower_all_io_to_elements ||
       nir->info.stage == MESA_SHADER_VERTEX ||
       nir->info.stage == MESA_SHADER_GEOMETRY) {
      NIR_PASS_V(nir, nir_lower_io_arrays_to_elements_no_indirects, false);
   } else if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      NIR_PASS_V(nir, nir_lower_io_arrays_to_elements_no_indirects, true);
   }

   st_nir_assign_vs_in_locations(nir);
}

/**
 * Translate ARB (asm) program to NIR
 */
static nir_shader *
st_translate_prog_to_nir(struct st_context *st, struct gl_program *prog,
                         gl_shader_stage stage)
{
   struct pipe_screen *screen = st->pipe->screen;
   const struct gl_shader_compiler_options *options =
      &st->ctx->Const.ShaderCompilerOptions[stage];

   /* Translate to NIR */
   nir_shader *nir = prog_to_nir(prog, options->NirOptions);
   NIR_PASS_V(nir, nir_lower_regs_to_ssa); /* turn registers into SSA */
   nir_validate_shader(nir, "after st/ptn lower_regs_to_ssa");

   NIR_PASS_V(nir, st_nir_lower_wpos_ytransform, prog, screen);
   NIR_PASS_V(nir, nir_lower_system_values);

   /* Optimise NIR */
   NIR_PASS_V(nir, nir_opt_constant_folding);
   st_nir_opts(nir);
   st_finalize_nir_before_variants(nir);

   if (st->allow_st_finalize_nir_twice)
      st_finalize_nir(st, prog, NULL, nir, true);

   nir_validate_shader(nir, "after st/glsl finalize_nir");

   return nir;
}

void
st_prepare_vertex_program(struct st_program *stp)
{
   struct st_vertex_program *stvp = (struct st_vertex_program *)stp;

   stvp->num_inputs = 0;
   memset(stvp->input_to_index, ~0, sizeof(stvp->input_to_index));
   memset(stvp->result_to_output, ~0, sizeof(stvp->result_to_output));

   /* Determine number of inputs, the mappings between VERT_ATTRIB_x
    * and TGSI generic input indexes, plus input attrib semantic info.
    */
   for (unsigned attr = 0; attr < VERT_ATTRIB_MAX; attr++) {
      if ((stp->Base.info.inputs_read & BITFIELD64_BIT(attr)) != 0) {
         stvp->input_to_index[attr] = stvp->num_inputs;
         stvp->index_to_input[stvp->num_inputs] = attr;
         stvp->num_inputs++;

         if ((stp->Base.DualSlotInputs & BITFIELD64_BIT(attr)) != 0) {
            /* add placeholder for second part of a double attribute */
            stvp->index_to_input[stvp->num_inputs] = ST_DOUBLE_ATTRIB_PLACEHOLDER;
            stvp->num_inputs++;
         }
      }
   }
   /* pre-setup potentially unused edgeflag input */
   stvp->input_to_index[VERT_ATTRIB_EDGEFLAG] = stvp->num_inputs;
   stvp->index_to_input[stvp->num_inputs] = VERT_ATTRIB_EDGEFLAG;

   /* Compute mapping of vertex program outputs to slots. */
   unsigned num_outputs = 0;
   for (unsigned attr = 0; attr < VARYING_SLOT_MAX; attr++) {
      if (stp->Base.info.outputs_written & BITFIELD64_BIT(attr))
         stvp->result_to_output[attr] = num_outputs++;
   }
   /* pre-setup potentially unused edgeflag output */
   stvp->result_to_output[VARYING_SLOT_EDGE] = num_outputs;
}

void
st_translate_stream_output_info(struct gl_program *prog)
{
   struct gl_transform_feedback_info *info = prog->sh.LinkedTransformFeedback;
   if (!info)
      return;

   /* Determine the (default) output register mapping for each output. */
   unsigned num_outputs = 0;
   ubyte output_mapping[VARYING_SLOT_TESS_MAX];
   memset(output_mapping, 0, sizeof(output_mapping));

   for (unsigned attr = 0; attr < VARYING_SLOT_MAX; attr++) {
      if (prog->info.outputs_written & BITFIELD64_BIT(attr))
         output_mapping[attr] = num_outputs++;
   }

   /* Translate stream output info. */
   struct pipe_stream_output_info *so_info =
      &((struct st_program*)prog)->state.stream_output;

   for (unsigned i = 0; i < info->NumOutputs; i++) {
      so_info->output[i].register_index =
         output_mapping[info->Outputs[i].OutputRegister];
      so_info->output[i].start_component = info->Outputs[i].ComponentOffset;
      so_info->output[i].num_components = info->Outputs[i].NumComponents;
      so_info->output[i].output_buffer = info->Outputs[i].OutputBuffer;
      so_info->output[i].dst_offset = info->Outputs[i].DstOffset;
      so_info->output[i].stream = info->Outputs[i].StreamId;
   }

   for (unsigned i = 0; i < PIPE_MAX_SO_BUFFERS; i++) {
      so_info->stride[i] = info->Buffers[i].Stride;
   }
   so_info->num_outputs = info->NumOutputs;
}

/**
 * Translate a vertex program.
 */
bool
st_translate_vertex_program(struct st_context *st,
                            struct st_program *stp)
{
   struct ureg_program *ureg;
   enum pipe_error error;
   unsigned num_outputs = 0;
   unsigned attr;
   ubyte output_semantic_name[VARYING_SLOT_MAX] = {0};
   ubyte output_semantic_index[VARYING_SLOT_MAX] = {0};

   if (stp->Base.arb.IsPositionInvariant)
      _mesa_insert_mvp_code(st->ctx, &stp->Base);

   st_prepare_vertex_program(stp);

   /* ARB_vp: */
   if (!stp->glsl_to_tgsi) {
      _mesa_remove_output_reads(&stp->Base, PROGRAM_OUTPUT);

      /* This determines which states will be updated when the assembly
       * shader is bound.
       */
      stp->affected_states = ST_NEW_VS_STATE |
                              ST_NEW_RASTERIZER |
                              ST_NEW_VERTEX_ARRAYS;

      if (stp->Base.Parameters->NumParameters)
         stp->affected_states |= ST_NEW_VS_CONSTANTS;

      /* Translate to NIR if preferred. */
      if (st->pipe->screen->get_shader_param(st->pipe->screen,
                                             PIPE_SHADER_VERTEX,
                                             PIPE_SHADER_CAP_PREFERRED_IR)) {
         assert(!stp->glsl_to_tgsi);

         if (stp->Base.nir)
            ralloc_free(stp->Base.nir);

         stp->state.type = PIPE_SHADER_IR_NIR;
         stp->Base.nir = st_translate_prog_to_nir(st, &stp->Base,
                                                  MESA_SHADER_VERTEX);
         /* For st_draw_feedback, we need to generate TGSI too if draw doesn't
          * use LLVM.
          */
         if (draw_has_llvm())
            return true;
      }
   }

   /* Get semantic names and indices. */
   for (attr = 0; attr < VARYING_SLOT_MAX; attr++) {
      if (stp->Base.info.outputs_written & BITFIELD64_BIT(attr)) {
         unsigned slot = num_outputs++;
         unsigned semantic_name, semantic_index;
         tgsi_get_gl_varying_semantic(attr, st->needs_texcoord_semantic,
                                      &semantic_name, &semantic_index);
         output_semantic_name[slot] = semantic_name;
         output_semantic_index[slot] = semantic_index;
      }
   }
   /* pre-setup potentially unused edgeflag output */
   output_semantic_name[num_outputs] = TGSI_SEMANTIC_EDGEFLAG;
   output_semantic_index[num_outputs] = 0;

   ureg = ureg_create_with_screen(PIPE_SHADER_VERTEX, st->pipe->screen);
   if (ureg == NULL)
      return false;

   if (stp->Base.info.clip_distance_array_size)
      ureg_property(ureg, TGSI_PROPERTY_NUM_CLIPDIST_ENABLED,
                    stp->Base.info.clip_distance_array_size);
   if (stp->Base.info.cull_distance_array_size)
      ureg_property(ureg, TGSI_PROPERTY_NUM_CULLDIST_ENABLED,
                    stp->Base.info.cull_distance_array_size);

   if (ST_DEBUG & DEBUG_MESA) {
      _mesa_print_program(&stp->Base);
      _mesa_print_program_parameters(st->ctx, &stp->Base);
      debug_printf("\n");
   }

   struct st_vertex_program *stvp = (struct st_vertex_program *)stp;

   if (stp->glsl_to_tgsi) {
      error = st_translate_program(st->ctx,
                                   PIPE_SHADER_VERTEX,
                                   ureg,
                                   stp->glsl_to_tgsi,
                                   &stp->Base,
                                   /* inputs */
                                   stvp->num_inputs,
                                   stvp->input_to_index,
                                   NULL, /* inputSlotToAttr */
                                   NULL, /* input semantic name */
                                   NULL, /* input semantic index */
                                   NULL, /* interp mode */
                                   /* outputs */
                                   num_outputs,
                                   stvp->result_to_output,
                                   output_semantic_name,
                                   output_semantic_index);

      st_translate_stream_output_info(&stp->Base);

      free_glsl_to_tgsi_visitor(stp->glsl_to_tgsi);
   } else
      error = st_translate_mesa_program(st->ctx,
                                        PIPE_SHADER_VERTEX,
                                        ureg,
                                        &stp->Base,
                                        /* inputs */
                                        stvp->num_inputs,
                                        stvp->input_to_index,
                                        NULL, /* input semantic name */
                                        NULL, /* input semantic index */
                                        NULL,
                                        /* outputs */
                                        num_outputs,
                                        stvp->result_to_output,
                                        output_semantic_name,
                                        output_semantic_index);

   if (error) {
      debug_printf("%s: failed to translate Mesa program:\n", __func__);
      _mesa_print_program(&stp->Base);
      debug_assert(0);
      return false;
   }

   stp->state.tokens = ureg_get_tokens(ureg, NULL);
   ureg_destroy(ureg);

   if (stp->glsl_to_tgsi) {
      stp->glsl_to_tgsi = NULL;
      st_store_ir_in_disk_cache(st, &stp->Base, false);
   }

   return stp->state.tokens != NULL;
}

static const gl_state_index16 depth_range_state[STATE_LENGTH] =
   { STATE_DEPTH_RANGE };

static struct st_common_variant *
st_create_vp_variant(struct st_context *st,
                     struct st_program *stvp,
                     const struct st_common_variant_key *key)
{
   struct st_common_variant *vpv = CALLOC_STRUCT(st_common_variant);
   struct pipe_context *pipe = st->pipe;
   struct pipe_screen *screen = pipe->screen;
   struct pipe_shader_state state = {0};

   static const gl_state_index16 point_size_state[STATE_LENGTH] =
      { STATE_INTERNAL, STATE_POINT_SIZE_CLAMPED, 0 };
   struct gl_program_parameter_list *params = stvp->Base.Parameters;

   vpv->key = *key;

   state.stream_output = stvp->state.stream_output;

   if (stvp->state.type == PIPE_SHADER_IR_NIR &&
       (!key->is_draw_shader || draw_has_llvm())) {
      bool finalize = false;

      state.type = PIPE_SHADER_IR_NIR;
      state.ir.nir = nir_shader_clone(NULL, stvp->Base.nir);
      if (key->clamp_color) {
         NIR_PASS_V(state.ir.nir, nir_lower_clamp_color_outputs);
         finalize = true;
      }
      if (key->passthrough_edgeflags) {
         NIR_PASS_V(state.ir.nir, nir_lower_passthrough_edgeflags);
         finalize = true;
      }

      if (key->lower_point_size) {
         _mesa_add_state_reference(params, point_size_state);
         NIR_PASS_V(state.ir.nir, nir_lower_point_size_mov,
                    point_size_state);
         finalize = true;
      }

      if (key->lower_ucp) {
         bool can_compact = screen->get_param(screen,
                                              PIPE_CAP_NIR_COMPACT_ARRAYS);

         bool use_eye = st->ctx->_Shader->CurrentProgram[MESA_SHADER_VERTEX] != NULL;
         gl_state_index16 clipplane_state[MAX_CLIP_PLANES][STATE_LENGTH];
         for (int i = 0; i < MAX_CLIP_PLANES; ++i) {
            if (use_eye) {
               clipplane_state[i][0] = STATE_CLIPPLANE;
               clipplane_state[i][1] = i;
            } else {
               clipplane_state[i][0] = STATE_INTERNAL;
               clipplane_state[i][1] = STATE_CLIP_INTERNAL;
               clipplane_state[i][2] = i;
            }
            _mesa_add_state_reference(params, clipplane_state[i]);
         }

         NIR_PASS_V(state.ir.nir, nir_lower_clip_vs, key->lower_ucp,
                    true, can_compact, clipplane_state);
         NIR_PASS_V(state.ir.nir, nir_lower_io_to_temporaries,
                    nir_shader_get_entrypoint(state.ir.nir), true, false);
         NIR_PASS_V(state.ir.nir, nir_lower_global_vars_to_local);
         finalize = true;
      }

      if (finalize || !st->allow_st_finalize_nir_twice) {
         st_finalize_nir(st, &stvp->Base, stvp->shader_program, state.ir.nir,
                         true);

         /* Some of the lowering above may have introduced new varyings */
         nir_shader_gather_info(state.ir.nir,
                                nir_shader_get_entrypoint(state.ir.nir));
      }

      if (ST_DEBUG & DEBUG_PRINT_IR)
         nir_print_shader(state.ir.nir, stderr);

      if (key->is_draw_shader)
         vpv->base.driver_shader = draw_create_vertex_shader(st->draw, &state);
      else
         vpv->base.driver_shader = pipe->create_vs_state(pipe, &state);

      return vpv;
   }

   state.type = PIPE_SHADER_IR_TGSI;
   state.tokens = tgsi_dup_tokens(stvp->state.tokens);

   /* Emulate features. */
   if (key->clamp_color || key->passthrough_edgeflags) {
      const struct tgsi_token *tokens;
      unsigned flags =
         (key->clamp_color ? TGSI_EMU_CLAMP_COLOR_OUTPUTS : 0) |
         (key->passthrough_edgeflags ? TGSI_EMU_PASSTHROUGH_EDGEFLAG : 0);

      tokens = tgsi_emulate(state.tokens, flags);

      if (tokens) {
         tgsi_free_tokens(state.tokens);
         state.tokens = tokens;
      } else {
         fprintf(stderr, "mesa: cannot emulate deprecated features\n");
      }
   }

   if (key->lower_depth_clamp) {
      unsigned depth_range_const =
            _mesa_add_state_reference(params, depth_range_state);

      const struct tgsi_token *tokens;
      tokens = st_tgsi_lower_depth_clamp(state.tokens, depth_range_const,
                                         key->clip_negative_one_to_one);
      if (tokens != state.tokens)
         tgsi_free_tokens(state.tokens);
      state.tokens = tokens;
   }

   if (ST_DEBUG & DEBUG_PRINT_IR)
      tgsi_dump(state.tokens, 0);

   if (key->is_draw_shader)
      vpv->base.driver_shader = draw_create_vertex_shader(st->draw, &state);
   else
      vpv->base.driver_shader = pipe->create_vs_state(pipe, &state);

   if (state.tokens) {
      tgsi_free_tokens(state.tokens);
   }

   return vpv;
}


/**
 * Find/create a vertex program variant.
 */
struct st_common_variant *
st_get_vp_variant(struct st_context *st,
                  struct st_program *stp,
                  const struct st_common_variant_key *key)
{
   struct st_vertex_program *stvp = (struct st_vertex_program *)stp;
   struct st_common_variant *vpv;

   /* Search for existing variant */
   for (vpv = st_common_variant(stp->variants); vpv;
        vpv = st_common_variant(vpv->base.next)) {
      if (memcmp(&vpv->key, key, sizeof(*key)) == 0) {
         break;
      }
   }

   if (!vpv) {
      /* create now */
      vpv = st_create_vp_variant(st, stp, key);
      if (vpv) {
         vpv->base.st = key->st;

         unsigned num_inputs = stvp->num_inputs + key->passthrough_edgeflags;
         for (unsigned index = 0; index < num_inputs; ++index) {
            unsigned attr = stvp->index_to_input[index];
            if (attr == ST_DOUBLE_ATTRIB_PLACEHOLDER)
               continue;
            vpv->vert_attrib_mask |= 1u << attr;
         }

         /* insert into list */
         vpv->base.next = stp->variants;
         stp->variants = &vpv->base;
      }
   }

   return vpv;
}


/**
 * Translate a Mesa fragment shader into a TGSI shader.
 */
bool
st_translate_fragment_program(struct st_context *st,
                              struct st_program *stfp)
{
   /* Non-GLSL programs: */
   if (!stfp->glsl_to_tgsi) {
      _mesa_remove_output_reads(&stfp->Base, PROGRAM_OUTPUT);
      if (st->ctx->Const.GLSLFragCoordIsSysVal)
         _mesa_program_fragment_position_to_sysval(&stfp->Base);

      /* This determines which states will be updated when the assembly
       * shader is bound.
       *
       * fragment.position and glDrawPixels always use constants.
       */
      stfp->affected_states = ST_NEW_FS_STATE |
                              ST_NEW_SAMPLE_SHADING |
                              ST_NEW_FS_CONSTANTS;

      if (stfp->ati_fs) {
         /* Just set them for ATI_fs unconditionally. */
         stfp->affected_states |= ST_NEW_FS_SAMPLER_VIEWS |
                                  ST_NEW_FS_SAMPLERS;
      } else {
         /* ARB_fp */
         if (stfp->Base.SamplersUsed)
            stfp->affected_states |= ST_NEW_FS_SAMPLER_VIEWS |
                                     ST_NEW_FS_SAMPLERS;
      }

      /* Translate to NIR. */
      if (!stfp->ati_fs &&
          st->pipe->screen->get_shader_param(st->pipe->screen,
                                             PIPE_SHADER_FRAGMENT,
                                             PIPE_SHADER_CAP_PREFERRED_IR)) {
         nir_shader *nir =
            st_translate_prog_to_nir(st, &stfp->Base, MESA_SHADER_FRAGMENT);

         if (stfp->Base.nir)
            ralloc_free(stfp->Base.nir);
         stfp->state.type = PIPE_SHADER_IR_NIR;
         stfp->Base.nir = nir;
         return true;
      }
   }

   ubyte outputMapping[2 * FRAG_RESULT_MAX];
   ubyte inputMapping[VARYING_SLOT_MAX];
   ubyte inputSlotToAttr[VARYING_SLOT_MAX];
   ubyte interpMode[PIPE_MAX_SHADER_INPUTS];  /* XXX size? */
   GLuint attr;
   GLbitfield64 inputsRead;
   struct ureg_program *ureg;

   GLboolean write_all = GL_FALSE;

   ubyte input_semantic_name[PIPE_MAX_SHADER_INPUTS];
   ubyte input_semantic_index[PIPE_MAX_SHADER_INPUTS];
   uint fs_num_inputs = 0;

   ubyte fs_output_semantic_name[PIPE_MAX_SHADER_OUTPUTS];
   ubyte fs_output_semantic_index[PIPE_MAX_SHADER_OUTPUTS];
   uint fs_num_outputs = 0;

   memset(inputSlotToAttr, ~0, sizeof(inputSlotToAttr));

   /*
    * Convert Mesa program inputs to TGSI input register semantics.
    */
   inputsRead = stfp->Base.info.inputs_read;
   for (attr = 0; attr < VARYING_SLOT_MAX; attr++) {
      if ((inputsRead & BITFIELD64_BIT(attr)) != 0) {
         const GLuint slot = fs_num_inputs++;

         inputMapping[attr] = slot;
         inputSlotToAttr[slot] = attr;

         switch (attr) {
         case VARYING_SLOT_POS:
            input_semantic_name[slot] = TGSI_SEMANTIC_POSITION;
            input_semantic_index[slot] = 0;
            interpMode[slot] = TGSI_INTERPOLATE_LINEAR;
            break;
         case VARYING_SLOT_COL0:
            input_semantic_name[slot] = TGSI_SEMANTIC_COLOR;
            input_semantic_index[slot] = 0;
            interpMode[slot] = stfp->glsl_to_tgsi ?
               TGSI_INTERPOLATE_COUNT : TGSI_INTERPOLATE_COLOR;
            break;
         case VARYING_SLOT_COL1:
            input_semantic_name[slot] = TGSI_SEMANTIC_COLOR;
            input_semantic_index[slot] = 1;
            interpMode[slot] = stfp->glsl_to_tgsi ?
               TGSI_INTERPOLATE_COUNT : TGSI_INTERPOLATE_COLOR;
            break;
         case VARYING_SLOT_FOGC:
            input_semantic_name[slot] = TGSI_SEMANTIC_FOG;
            input_semantic_index[slot] = 0;
            interpMode[slot] = TGSI_INTERPOLATE_PERSPECTIVE;
            break;
         case VARYING_SLOT_FACE:
            input_semantic_name[slot] = TGSI_SEMANTIC_FACE;
            input_semantic_index[slot] = 0;
            interpMode[slot] = TGSI_INTERPOLATE_CONSTANT;
            break;
         case VARYING_SLOT_PRIMITIVE_ID:
            input_semantic_name[slot] = TGSI_SEMANTIC_PRIMID;
            input_semantic_index[slot] = 0;
            interpMode[slot] = TGSI_INTERPOLATE_CONSTANT;
            break;
         case VARYING_SLOT_LAYER:
            input_semantic_name[slot] = TGSI_SEMANTIC_LAYER;
            input_semantic_index[slot] = 0;
            interpMode[slot] = TGSI_INTERPOLATE_CONSTANT;
            break;
         case VARYING_SLOT_VIEWPORT:
            input_semantic_name[slot] = TGSI_SEMANTIC_VIEWPORT_INDEX;
            input_semantic_index[slot] = 0;
            interpMode[slot] = TGSI_INTERPOLATE_CONSTANT;
            break;
         case VARYING_SLOT_CLIP_DIST0:
            input_semantic_name[slot] = TGSI_SEMANTIC_CLIPDIST;
            input_semantic_index[slot] = 0;
            interpMode[slot] = TGSI_INTERPOLATE_PERSPECTIVE;
            break;
         case VARYING_SLOT_CLIP_DIST1:
            input_semantic_name[slot] = TGSI_SEMANTIC_CLIPDIST;
            input_semantic_index[slot] = 1;
            interpMode[slot] = TGSI_INTERPOLATE_PERSPECTIVE;
            break;
         case VARYING_SLOT_CULL_DIST0:
         case VARYING_SLOT_CULL_DIST1:
            /* these should have been lowered by GLSL */
            assert(0);
            break;
            /* In most cases, there is nothing special about these
             * inputs, so adopt a convention to use the generic
             * semantic name and the mesa VARYING_SLOT_ number as the
             * index.
             *
             * All that is required is that the vertex shader labels
             * its own outputs similarly, and that the vertex shader
             * generates at least every output required by the
             * fragment shader plus fixed-function hardware (such as
             * BFC).
             *
             * However, some drivers may need us to identify the PNTC and TEXi
             * varyings if, for example, their capability to replace them with
             * sprite coordinates is limited.
             */
         case VARYING_SLOT_PNTC:
            if (st->needs_texcoord_semantic) {
               input_semantic_name[slot] = TGSI_SEMANTIC_PCOORD;
               input_semantic_index[slot] = 0;
               interpMode[slot] = TGSI_INTERPOLATE_LINEAR;
               break;
            }
            /* fall through */
         case VARYING_SLOT_TEX0:
         case VARYING_SLOT_TEX1:
         case VARYING_SLOT_TEX2:
         case VARYING_SLOT_TEX3:
         case VARYING_SLOT_TEX4:
         case VARYING_SLOT_TEX5:
         case VARYING_SLOT_TEX6:
         case VARYING_SLOT_TEX7:
            if (st->needs_texcoord_semantic) {
               input_semantic_name[slot] = TGSI_SEMANTIC_TEXCOORD;
               input_semantic_index[slot] = attr - VARYING_SLOT_TEX0;
               interpMode[slot] = stfp->glsl_to_tgsi ?
                  TGSI_INTERPOLATE_COUNT : TGSI_INTERPOLATE_PERSPECTIVE;
               break;
            }
            /* fall through */
         case VARYING_SLOT_VAR0:
         default:
            /* Semantic indices should be zero-based because drivers may choose
             * to assign a fixed slot determined by that index.
             * This is useful because ARB_separate_shader_objects uses location
             * qualifiers for linkage, and if the semantic index corresponds to
             * these locations, linkage passes in the driver become unecessary.
             *
             * If needs_texcoord_semantic is true, no semantic indices will be
             * consumed for the TEXi varyings, and we can base the locations of
             * the user varyings on VAR0.  Otherwise, we use TEX0 as base index.
             */
            assert(attr >= VARYING_SLOT_VAR0 || attr == VARYING_SLOT_PNTC ||
                   (attr >= VARYING_SLOT_TEX0 && attr <= VARYING_SLOT_TEX7));
            input_semantic_name[slot] = TGSI_SEMANTIC_GENERIC;
            input_semantic_index[slot] = st_get_generic_varying_index(st, attr);
            if (attr == VARYING_SLOT_PNTC)
               interpMode[slot] = TGSI_INTERPOLATE_LINEAR;
            else {
               interpMode[slot] = stfp->glsl_to_tgsi ?
                  TGSI_INTERPOLATE_COUNT : TGSI_INTERPOLATE_PERSPECTIVE;
            }
            break;
         }
      }
      else {
         inputMapping[attr] = -1;
      }
   }

   /*
    * Semantics and mapping for outputs
    */
   GLbitfield64 outputsWritten = stfp->Base.info.outputs_written;

   /* if z is written, emit that first */
   if (outputsWritten & BITFIELD64_BIT(FRAG_RESULT_DEPTH)) {
      fs_output_semantic_name[fs_num_outputs] = TGSI_SEMANTIC_POSITION;
      fs_output_semantic_index[fs_num_outputs] = 0;
      outputMapping[FRAG_RESULT_DEPTH] = fs_num_outputs;
      fs_num_outputs++;
      outputsWritten &= ~(1 << FRAG_RESULT_DEPTH);
   }

   if (outputsWritten & BITFIELD64_BIT(FRAG_RESULT_STENCIL)) {
      fs_output_semantic_name[fs_num_outputs] = TGSI_SEMANTIC_STENCIL;
      fs_output_semantic_index[fs_num_outputs] = 0;
      outputMapping[FRAG_RESULT_STENCIL] = fs_num_outputs;
      fs_num_outputs++;
      outputsWritten &= ~(1 << FRAG_RESULT_STENCIL);
   }

   if (outputsWritten & BITFIELD64_BIT(FRAG_RESULT_SAMPLE_MASK)) {
      fs_output_semantic_name[fs_num_outputs] = TGSI_SEMANTIC_SAMPLEMASK;
      fs_output_semantic_index[fs_num_outputs] = 0;
      outputMapping[FRAG_RESULT_SAMPLE_MASK] = fs_num_outputs;
      fs_num_outputs++;
      outputsWritten &= ~(1 << FRAG_RESULT_SAMPLE_MASK);
   }

   /* handle remaining outputs (color) */
   for (attr = 0; attr < ARRAY_SIZE(outputMapping); attr++) {
      const GLbitfield64 written = attr < FRAG_RESULT_MAX ? outputsWritten :
         stfp->Base.SecondaryOutputsWritten;
      const unsigned loc = attr % FRAG_RESULT_MAX;

      if (written & BITFIELD64_BIT(loc)) {
         switch (loc) {
         case FRAG_RESULT_DEPTH:
         case FRAG_RESULT_STENCIL:
         case FRAG_RESULT_SAMPLE_MASK:
            /* handled above */
            assert(0);
            break;
         case FRAG_RESULT_COLOR:
            write_all = GL_TRUE; /* fallthrough */
         default: {
            int index;
            assert(loc == FRAG_RESULT_COLOR ||
                   (FRAG_RESULT_DATA0 <= loc && loc < FRAG_RESULT_MAX));

            index = (loc == FRAG_RESULT_COLOR) ? 0 : (loc - FRAG_RESULT_DATA0);

            if (attr >= FRAG_RESULT_MAX) {
               /* Secondary color for dual source blending. */
               assert(index == 0);
               index++;
            }

            fs_output_semantic_name[fs_num_outputs] = TGSI_SEMANTIC_COLOR;
            fs_output_semantic_index[fs_num_outputs] = index;
            outputMapping[attr] = fs_num_outputs;
            break;
         }
         }

         fs_num_outputs++;
      }
   }

   ureg = ureg_create_with_screen(PIPE_SHADER_FRAGMENT, st->pipe->screen);
   if (ureg == NULL)
      return false;

   if (ST_DEBUG & DEBUG_MESA) {
      _mesa_print_program(&stfp->Base);
      _mesa_print_program_parameters(st->ctx, &stfp->Base);
      debug_printf("\n");
   }
   if (write_all == GL_TRUE)
      ureg_property(ureg, TGSI_PROPERTY_FS_COLOR0_WRITES_ALL_CBUFS, 1);

   if (stfp->Base.info.fs.depth_layout != FRAG_DEPTH_LAYOUT_NONE) {
      switch (stfp->Base.info.fs.depth_layout) {
      case FRAG_DEPTH_LAYOUT_ANY:
         ureg_property(ureg, TGSI_PROPERTY_FS_DEPTH_LAYOUT,
                       TGSI_FS_DEPTH_LAYOUT_ANY);
         break;
      case FRAG_DEPTH_LAYOUT_GREATER:
         ureg_property(ureg, TGSI_PROPERTY_FS_DEPTH_LAYOUT,
                       TGSI_FS_DEPTH_LAYOUT_GREATER);
         break;
      case FRAG_DEPTH_LAYOUT_LESS:
         ureg_property(ureg, TGSI_PROPERTY_FS_DEPTH_LAYOUT,
                       TGSI_FS_DEPTH_LAYOUT_LESS);
         break;
      case FRAG_DEPTH_LAYOUT_UNCHANGED:
         ureg_property(ureg, TGSI_PROPERTY_FS_DEPTH_LAYOUT,
                       TGSI_FS_DEPTH_LAYOUT_UNCHANGED);
         break;
      default:
         assert(0);
      }
   }

   if (stfp->glsl_to_tgsi) {
      st_translate_program(st->ctx,
                           PIPE_SHADER_FRAGMENT,
                           ureg,
                           stfp->glsl_to_tgsi,
                           &stfp->Base,
                           /* inputs */
                           fs_num_inputs,
                           inputMapping,
                           inputSlotToAttr,
                           input_semantic_name,
                           input_semantic_index,
                           interpMode,
                           /* outputs */
                           fs_num_outputs,
                           outputMapping,
                           fs_output_semantic_name,
                           fs_output_semantic_index);

      free_glsl_to_tgsi_visitor(stfp->glsl_to_tgsi);
   } else if (stfp->ati_fs)
      st_translate_atifs_program(ureg,
                                 stfp->ati_fs,
                                 &stfp->Base,
                                 /* inputs */
                                 fs_num_inputs,
                                 inputMapping,
                                 input_semantic_name,
                                 input_semantic_index,
                                 interpMode,
                                 /* outputs */
                                 fs_num_outputs,
                                 outputMapping,
                                 fs_output_semantic_name,
                                 fs_output_semantic_index);
   else
      st_translate_mesa_program(st->ctx,
                                PIPE_SHADER_FRAGMENT,
                                ureg,
                                &stfp->Base,
                                /* inputs */
                                fs_num_inputs,
                                inputMapping,
                                input_semantic_name,
                                input_semantic_index,
                                interpMode,
                                /* outputs */
                                fs_num_outputs,
                                outputMapping,
                                fs_output_semantic_name,
                                fs_output_semantic_index);

   stfp->state.tokens = ureg_get_tokens(ureg, NULL);
   ureg_destroy(ureg);

   if (stfp->glsl_to_tgsi) {
      stfp->glsl_to_tgsi = NULL;
      st_store_ir_in_disk_cache(st, &stfp->Base, false);
   }

   return stfp->state.tokens != NULL;
}

static struct st_fp_variant *
st_create_fp_variant(struct st_context *st,
                     struct st_program *stfp,
                     const struct st_fp_variant_key *key)
{
   struct pipe_context *pipe = st->pipe;
   struct st_fp_variant *variant = CALLOC_STRUCT(st_fp_variant);
   struct pipe_shader_state state = {0};
   struct gl_program_parameter_list *params = stfp->Base.Parameters;
   static const gl_state_index16 texcoord_state[STATE_LENGTH] =
      { STATE_INTERNAL, STATE_CURRENT_ATTRIB, VERT_ATTRIB_TEX0 };
   static const gl_state_index16 scale_state[STATE_LENGTH] =
      { STATE_INTERNAL, STATE_PT_SCALE };
   static const gl_state_index16 bias_state[STATE_LENGTH] =
      { STATE_INTERNAL, STATE_PT_BIAS };
   static const gl_state_index16 alpha_ref_state[STATE_LENGTH] =
      { STATE_INTERNAL, STATE_ALPHA_REF };

   if (!variant)
      return NULL;

   if (stfp->state.type == PIPE_SHADER_IR_NIR) {
      bool finalize = false;

      state.type = PIPE_SHADER_IR_NIR;
      state.ir.nir = nir_shader_clone(NULL, stfp->Base.nir);

      if (key->clamp_color) {
         NIR_PASS_V(state.ir.nir, nir_lower_clamp_color_outputs);
         finalize = true;
      }

      if (key->lower_flatshade) {
         NIR_PASS_V(state.ir.nir, nir_lower_flatshade);
         finalize = true;
      }

      if (key->lower_alpha_func != COMPARE_FUNC_NEVER) {
         _mesa_add_state_reference(params, alpha_ref_state);
         NIR_PASS_V(state.ir.nir, nir_lower_alpha_test, key->lower_alpha_func,
                    false, alpha_ref_state);
         finalize = true;
      }

      if (key->lower_two_sided_color) {
         NIR_PASS_V(state.ir.nir, nir_lower_two_sided_color);
         finalize = true;
      }

      if (key->persample_shading) {
          nir_shader *shader = state.ir.nir;
          nir_foreach_variable(var, &shader->inputs)
             var->data.sample = true;
          finalize = true;
      }

      assert(!(key->bitmap && key->drawpixels));

      /* glBitmap */
      if (key->bitmap) {
         nir_lower_bitmap_options options = {0};

         variant->bitmap_sampler = ffs(~stfp->Base.SamplersUsed) - 1;
         options.sampler = variant->bitmap_sampler;
         options.swizzle_xxxx = st->bitmap.tex_format == PIPE_FORMAT_R8_UNORM;

         NIR_PASS_V(state.ir.nir, nir_lower_bitmap, &options);
         finalize = true;
      }

      /* glDrawPixels (color only) */
      if (key->drawpixels) {
         nir_lower_drawpixels_options options = {{0}};
         unsigned samplers_used = stfp->Base.SamplersUsed;

         /* Find the first unused slot. */
         variant->drawpix_sampler = ffs(~samplers_used) - 1;
         options.drawpix_sampler = variant->drawpix_sampler;
         samplers_used |= (1 << variant->drawpix_sampler);

         options.pixel_maps = key->pixelMaps;
         if (key->pixelMaps) {
            variant->pixelmap_sampler = ffs(~samplers_used) - 1;
            options.pixelmap_sampler = variant->pixelmap_sampler;
         }

         options.scale_and_bias = key->scaleAndBias;
         if (key->scaleAndBias) {
            _mesa_add_state_reference(params, scale_state);
            memcpy(options.scale_state_tokens, scale_state,
                   sizeof(options.scale_state_tokens));
            _mesa_add_state_reference(params, bias_state);
            memcpy(options.bias_state_tokens, bias_state,
                   sizeof(options.bias_state_tokens));
         }

         _mesa_add_state_reference(params, texcoord_state);
         memcpy(options.texcoord_state_tokens, texcoord_state,
                sizeof(options.texcoord_state_tokens));

         NIR_PASS_V(state.ir.nir, nir_lower_drawpixels, &options);
         finalize = true;
      }

      if (unlikely(key->external.lower_nv12 || key->external.lower_iyuv ||
                   key->external.lower_xy_uxvx || key->external.lower_yx_xuxv ||
                   key->external.lower_ayuv || key->external.lower_xyuv)) {
         nir_lower_tex_options options = {0};
         options.lower_y_uv_external = key->external.lower_nv12;
         options.lower_y_u_v_external = key->external.lower_iyuv;
         options.lower_xy_uxvx_external = key->external.lower_xy_uxvx;
         options.lower_yx_xuxv_external = key->external.lower_yx_xuxv;
         options.lower_ayuv_external = key->external.lower_ayuv;
         options.lower_xyuv_external = key->external.lower_xyuv;
         NIR_PASS_V(state.ir.nir, nir_lower_tex, &options);
         finalize = true;
      }

      if (finalize || !st->allow_st_finalize_nir_twice) {
         st_finalize_nir(st, &stfp->Base, stfp->shader_program, state.ir.nir,
                         false);
      }

      /* This pass needs to happen *after* nir_lower_sampler */
      if (unlikely(key->external.lower_nv12 || key->external.lower_iyuv ||
                   key->external.lower_xy_uxvx || key->external.lower_yx_xuxv)) {
         NIR_PASS_V(state.ir.nir, st_nir_lower_tex_src_plane,
                    ~stfp->Base.SamplersUsed,
                    key->external.lower_nv12 || key->external.lower_xy_uxvx ||
                       key->external.lower_yx_xuxv,
                    key->external.lower_iyuv);
         finalize = true;
      }

      if (finalize || !st->allow_st_finalize_nir_twice) {
         /* Some of the lowering above may have introduced new varyings */
         nir_shader_gather_info(state.ir.nir,
                                nir_shader_get_entrypoint(state.ir.nir));

         struct pipe_screen *screen = pipe->screen;
         if (screen->finalize_nir)
            screen->finalize_nir(screen, state.ir.nir, false);
      }

      if (ST_DEBUG & DEBUG_PRINT_IR)
         nir_print_shader(state.ir.nir, stderr);

      variant->base.driver_shader = pipe->create_fs_state(pipe, &state);
      variant->key = *key;

      return variant;
   }

   state.tokens = stfp->state.tokens;

   assert(!(key->bitmap && key->drawpixels));

   /* Fix texture targets and add fog for ATI_fs */
   if (stfp->ati_fs) {
      const struct tgsi_token *tokens = st_fixup_atifs(state.tokens, key);

      if (tokens)
         state.tokens = tokens;
      else
         fprintf(stderr, "mesa: cannot post-process ATI_fs\n");
   }

   /* Emulate features. */
   if (key->clamp_color || key->persample_shading) {
      const struct tgsi_token *tokens;
      unsigned flags =
         (key->clamp_color ? TGSI_EMU_CLAMP_COLOR_OUTPUTS : 0) |
         (key->persample_shading ? TGSI_EMU_FORCE_PERSAMPLE_INTERP : 0);

      tokens = tgsi_emulate(state.tokens, flags);

      if (tokens) {
         if (state.tokens != stfp->state.tokens)
            tgsi_free_tokens(state.tokens);
         state.tokens = tokens;
      } else
         fprintf(stderr, "mesa: cannot emulate deprecated features\n");
   }

   /* glBitmap */
   if (key->bitmap) {
      const struct tgsi_token *tokens;

      variant->bitmap_sampler = ffs(~stfp->Base.SamplersUsed) - 1;

      tokens = st_get_bitmap_shader(state.tokens,
                                    st->internal_target,
                                    variant->bitmap_sampler,
                                    st->needs_texcoord_semantic,
                                    st->bitmap.tex_format ==
                                    PIPE_FORMAT_R8_UNORM);

      if (tokens) {
         if (state.tokens != stfp->state.tokens)
            tgsi_free_tokens(state.tokens);
         state.tokens = tokens;
      } else
         fprintf(stderr, "mesa: cannot create a shader for glBitmap\n");
   }

   /* glDrawPixels (color only) */
   if (key->drawpixels) {
      const struct tgsi_token *tokens;
      unsigned scale_const = 0, bias_const = 0, texcoord_const = 0;

      /* Find the first unused slot. */
      variant->drawpix_sampler = ffs(~stfp->Base.SamplersUsed) - 1;

      if (key->pixelMaps) {
         unsigned samplers_used = stfp->Base.SamplersUsed |
                                  (1 << variant->drawpix_sampler);

         variant->pixelmap_sampler = ffs(~samplers_used) - 1;
      }

      if (key->scaleAndBias) {
         scale_const = _mesa_add_state_reference(params, scale_state);
         bias_const = _mesa_add_state_reference(params, bias_state);
      }

      texcoord_const = _mesa_add_state_reference(params, texcoord_state);

      tokens = st_get_drawpix_shader(state.tokens,
                                     st->needs_texcoord_semantic,
                                     key->scaleAndBias, scale_const,
                                     bias_const, key->pixelMaps,
                                     variant->drawpix_sampler,
                                     variant->pixelmap_sampler,
                                     texcoord_const, st->internal_target);

      if (tokens) {
         if (state.tokens != stfp->state.tokens)
            tgsi_free_tokens(state.tokens);
         state.tokens = tokens;
      } else
         fprintf(stderr, "mesa: cannot create a shader for glDrawPixels\n");
   }

   if (unlikely(key->external.lower_nv12 || key->external.lower_iyuv ||
                key->external.lower_xy_uxvx || key->external.lower_yx_xuxv)) {
      const struct tgsi_token *tokens;

      /* samplers inserted would conflict, but this should be unpossible: */
      assert(!(key->bitmap || key->drawpixels));

      tokens = st_tgsi_lower_yuv(state.tokens,
                                 ~stfp->Base.SamplersUsed,
                                 key->external.lower_nv12 ||
                                    key->external.lower_xy_uxvx ||
                                    key->external.lower_yx_xuxv,
                                 key->external.lower_iyuv);
      if (tokens) {
         if (state.tokens != stfp->state.tokens)
            tgsi_free_tokens(state.tokens);
         state.tokens = tokens;
      } else {
         fprintf(stderr, "mesa: cannot create a shader for samplerExternalOES\n");
      }
   }

   if (key->lower_depth_clamp) {
      unsigned depth_range_const = _mesa_add_state_reference(params, depth_range_state);

      const struct tgsi_token *tokens;
      tokens = st_tgsi_lower_depth_clamp_fs(state.tokens, depth_range_const);
      if (state.tokens != stfp->state.tokens)
         tgsi_free_tokens(state.tokens);
      state.tokens = tokens;
   }

   if (ST_DEBUG & DEBUG_PRINT_IR)
      tgsi_dump(state.tokens, 0);

   /* fill in variant */
   variant->base.driver_shader = pipe->create_fs_state(pipe, &state);
   variant->key = *key;

   if (state.tokens != stfp->state.tokens)
      tgsi_free_tokens(state.tokens);
   return variant;
}

/**
 * Translate fragment program if needed.
 */
struct st_fp_variant *
st_get_fp_variant(struct st_context *st,
                  struct st_program *stfp,
                  const struct st_fp_variant_key *key)
{
   struct st_fp_variant *fpv;

   /* Search for existing variant */
   for (fpv = st_fp_variant(stfp->variants); fpv;
        fpv = st_fp_variant(fpv->base.next)) {
      if (memcmp(&fpv->key, key, sizeof(*key)) == 0) {
         break;
      }
   }

   if (!fpv) {
      /* create new */
      fpv = st_create_fp_variant(st, stfp, key);
      if (fpv) {
         fpv->base.st = key->st;

         if (key->bitmap || key->drawpixels) {
            /* Regular variants should always come before the
             * bitmap & drawpixels variants, (unless there
             * are no regular variants) so that
             * st_update_fp can take a fast path when
             * shader_has_one_variant is set.
             */
            if (!stfp->variants) {
               stfp->variants = &fpv->base;
            } else {
               /* insert into list after the first one */
               fpv->base.next = stfp->variants->next;
               stfp->variants->next = &fpv->base;
            }
         } else {
            /* insert into list */
            fpv->base.next = stfp->variants;
            stfp->variants = &fpv->base;
         }
      }
   }

   return fpv;
}

/**
 * Translate a program. This is common code for geometry and tessellation
 * shaders.
 */
bool
st_translate_common_program(struct st_context *st,
                            struct st_program *stp)
{
   struct gl_program *prog = &stp->Base;
   enum pipe_shader_type stage =
      pipe_shader_type_from_mesa(stp->Base.info.stage);
   struct ureg_program *ureg = ureg_create_with_screen(stage, st->pipe->screen);

   if (ureg == NULL)
      return false;

   switch (stage) {
   case PIPE_SHADER_TESS_CTRL:
      ureg_property(ureg, TGSI_PROPERTY_TCS_VERTICES_OUT,
                    stp->Base.info.tess.tcs_vertices_out);
      break;

   case PIPE_SHADER_TESS_EVAL:
      if (stp->Base.info.tess.primitive_mode == GL_ISOLINES)
         ureg_property(ureg, TGSI_PROPERTY_TES_PRIM_MODE, GL_LINES);
      else
         ureg_property(ureg, TGSI_PROPERTY_TES_PRIM_MODE,
                       stp->Base.info.tess.primitive_mode);

      STATIC_ASSERT((TESS_SPACING_EQUAL + 1) % 3 == PIPE_TESS_SPACING_EQUAL);
      STATIC_ASSERT((TESS_SPACING_FRACTIONAL_ODD + 1) % 3 ==
                    PIPE_TESS_SPACING_FRACTIONAL_ODD);
      STATIC_ASSERT((TESS_SPACING_FRACTIONAL_EVEN + 1) % 3 ==
                    PIPE_TESS_SPACING_FRACTIONAL_EVEN);

      ureg_property(ureg, TGSI_PROPERTY_TES_SPACING,
                    (stp->Base.info.tess.spacing + 1) % 3);

      ureg_property(ureg, TGSI_PROPERTY_TES_VERTEX_ORDER_CW,
                    !stp->Base.info.tess.ccw);
      ureg_property(ureg, TGSI_PROPERTY_TES_POINT_MODE,
                    stp->Base.info.tess.point_mode);
      break;

   case PIPE_SHADER_GEOMETRY:
      ureg_property(ureg, TGSI_PROPERTY_GS_INPUT_PRIM,
                    stp->Base.info.gs.input_primitive);
      ureg_property(ureg, TGSI_PROPERTY_GS_OUTPUT_PRIM,
                    stp->Base.info.gs.output_primitive);
      ureg_property(ureg, TGSI_PROPERTY_GS_MAX_OUTPUT_VERTICES,
                    stp->Base.info.gs.vertices_out);
      ureg_property(ureg, TGSI_PROPERTY_GS_INVOCATIONS,
                    stp->Base.info.gs.invocations);
      break;

   default:
      break;
   }

   ubyte inputSlotToAttr[VARYING_SLOT_TESS_MAX];
   ubyte inputMapping[VARYING_SLOT_TESS_MAX];
   ubyte outputMapping[VARYING_SLOT_TESS_MAX];
   GLuint attr;

   ubyte input_semantic_name[PIPE_MAX_SHADER_INPUTS];
   ubyte input_semantic_index[PIPE_MAX_SHADER_INPUTS];
   uint num_inputs = 0;

   ubyte output_semantic_name[PIPE_MAX_SHADER_OUTPUTS];
   ubyte output_semantic_index[PIPE_MAX_SHADER_OUTPUTS];
   uint num_outputs = 0;

   GLint i;

   memset(inputSlotToAttr, 0, sizeof(inputSlotToAttr));
   memset(inputMapping, 0, sizeof(inputMapping));
   memset(outputMapping, 0, sizeof(outputMapping));
   memset(&stp->state, 0, sizeof(stp->state));

   if (prog->info.clip_distance_array_size)
      ureg_property(ureg, TGSI_PROPERTY_NUM_CLIPDIST_ENABLED,
                    prog->info.clip_distance_array_size);
   if (prog->info.cull_distance_array_size)
      ureg_property(ureg, TGSI_PROPERTY_NUM_CULLDIST_ENABLED,
                    prog->info.cull_distance_array_size);

   /*
    * Convert Mesa program inputs to TGSI input register semantics.
    */
   for (attr = 0; attr < VARYING_SLOT_MAX; attr++) {
      if ((prog->info.inputs_read & BITFIELD64_BIT(attr)) == 0)
         continue;

      unsigned slot = num_inputs++;

      inputMapping[attr] = slot;
      inputSlotToAttr[slot] = attr;

      unsigned semantic_name, semantic_index;
      tgsi_get_gl_varying_semantic(attr, st->needs_texcoord_semantic,
                                   &semantic_name, &semantic_index);
      input_semantic_name[slot] = semantic_name;
      input_semantic_index[slot] = semantic_index;
   }

   /* Also add patch inputs. */
   for (attr = 0; attr < 32; attr++) {
      if (prog->info.patch_inputs_read & (1u << attr)) {
         GLuint slot = num_inputs++;
         GLuint patch_attr = VARYING_SLOT_PATCH0 + attr;

         inputMapping[patch_attr] = slot;
         inputSlotToAttr[slot] = patch_attr;
         input_semantic_name[slot] = TGSI_SEMANTIC_PATCH;
         input_semantic_index[slot] = attr;
      }
   }

   /* initialize output semantics to defaults */
   for (i = 0; i < PIPE_MAX_SHADER_OUTPUTS; i++) {
      output_semantic_name[i] = TGSI_SEMANTIC_GENERIC;
      output_semantic_index[i] = 0;
   }

   /*
    * Determine number of outputs, the (default) output register
    * mapping and the semantic information for each output.
    */
   for (attr = 0; attr < VARYING_SLOT_MAX; attr++) {
      if (prog->info.outputs_written & BITFIELD64_BIT(attr)) {
         GLuint slot = num_outputs++;

         outputMapping[attr] = slot;

         unsigned semantic_name, semantic_index;
         tgsi_get_gl_varying_semantic(attr, st->needs_texcoord_semantic,
                                      &semantic_name, &semantic_index);
         output_semantic_name[slot] = semantic_name;
         output_semantic_index[slot] = semantic_index;
      }
   }

   /* Also add patch outputs. */
   for (attr = 0; attr < 32; attr++) {
      if (prog->info.patch_outputs_written & (1u << attr)) {
         GLuint slot = num_outputs++;
         GLuint patch_attr = VARYING_SLOT_PATCH0 + attr;

         outputMapping[patch_attr] = slot;
         output_semantic_name[slot] = TGSI_SEMANTIC_PATCH;
         output_semantic_index[slot] = attr;
      }
   }

   st_translate_program(st->ctx,
                        stage,
                        ureg,
                        stp->glsl_to_tgsi,
                        prog,
                        /* inputs */
                        num_inputs,
                        inputMapping,
                        inputSlotToAttr,
                        input_semantic_name,
                        input_semantic_index,
                        NULL,
                        /* outputs */
                        num_outputs,
                        outputMapping,
                        output_semantic_name,
                        output_semantic_index);

   stp->state.tokens = ureg_get_tokens(ureg, NULL);

   ureg_destroy(ureg);

   st_translate_stream_output_info(prog);

   st_store_ir_in_disk_cache(st, prog, false);

   if (ST_DEBUG & DEBUG_PRINT_IR && ST_DEBUG & DEBUG_MESA)
      _mesa_print_program(prog);

   free_glsl_to_tgsi_visitor(stp->glsl_to_tgsi);
   stp->glsl_to_tgsi = NULL;
   return true;
}


/**
 * Get/create a basic program variant.
 */
struct st_variant *
st_get_common_variant(struct st_context *st,
                      struct st_program *prog,
                      const struct st_common_variant_key *key)
{
   struct pipe_context *pipe = st->pipe;
   struct st_variant *v;
   struct pipe_shader_state state = {0};

   /* Search for existing variant */
   for (v = prog->variants; v; v = v->next) {
      if (memcmp(&st_common_variant(v)->key, key, sizeof(*key)) == 0)
         break;
   }

   if (!v) {
      /* create new */
      v = (struct st_variant*)CALLOC_STRUCT(st_common_variant);
      if (v) {
	 if (prog->state.type == PIPE_SHADER_IR_NIR) {
            bool finalize = false;

	    state.type = PIPE_SHADER_IR_NIR;
	    state.ir.nir = nir_shader_clone(NULL, prog->Base.nir);

            if (key->clamp_color) {
               NIR_PASS_V(state.ir.nir, nir_lower_clamp_color_outputs);
               finalize = true;
            }

            state.stream_output = prog->state.stream_output;

            if (finalize || !st->allow_st_finalize_nir_twice) {
               st_finalize_nir(st, &prog->Base, prog->shader_program,
                               state.ir.nir, true);
            }

            if (ST_DEBUG & DEBUG_PRINT_IR)
               nir_print_shader(state.ir.nir, stderr);
         } else {
            if (key->lower_depth_clamp) {
               struct gl_program_parameter_list *params = prog->Base.Parameters;

               unsigned depth_range_const =
                     _mesa_add_state_reference(params, depth_range_state);

               const struct tgsi_token *tokens;
               tokens =
                     st_tgsi_lower_depth_clamp(prog->state.tokens,
                                               depth_range_const,
                                               key->clip_negative_one_to_one);

               if (tokens != prog->state.tokens)
                  tgsi_free_tokens(prog->state.tokens);

               prog->state.tokens = tokens;
            }
            state = prog->state;

            if (ST_DEBUG & DEBUG_PRINT_IR)
               tgsi_dump(state.tokens, 0);
         }
         /* fill in new variant */
         switch (prog->Base.info.stage) {
         case MESA_SHADER_TESS_CTRL:
            v->driver_shader = pipe->create_tcs_state(pipe, &state);
            break;
         case MESA_SHADER_TESS_EVAL:
            v->driver_shader = pipe->create_tes_state(pipe, &state);
            break;
         case MESA_SHADER_GEOMETRY:
            v->driver_shader = pipe->create_gs_state(pipe, &state);
            break;
         case MESA_SHADER_COMPUTE: {
            struct pipe_compute_state cs = {0};
            cs.ir_type = state.type;
            cs.req_local_mem = prog->Base.info.cs.shared_size;

            if (state.type == PIPE_SHADER_IR_NIR)
               cs.prog = state.ir.nir;
            else
               cs.prog = state.tokens;

            v->driver_shader = pipe->create_compute_state(pipe, &cs);
            break;
         }
         default:
            assert(!"unhandled shader type");
            free(v);
            return NULL;
         }

         st_common_variant(v)->key = *key;
         v->st = key->st;

         /* insert into list */
         v->next = prog->variants;
         prog->variants = v;
      }
   }

   return v;
}


/**
 * Vert/Geom/Frag programs have per-context variants.  Free all the
 * variants attached to the given program which match the given context.
 */
static void
destroy_program_variants(struct st_context *st, struct gl_program *target)
{
   if (!target || target == &_mesa_DummyProgram)
      return;

   struct st_program *p = st_program(target);
   struct st_variant *v, **prevPtr = &p->variants;

   for (v = p->variants; v; ) {
      struct st_variant *next = v->next;
      if (v->st == st) {
         /* unlink from list */
         *prevPtr = next;
         /* destroy this variant */
         delete_variant(st, v, target->Target);
      }
      else {
         prevPtr = &v->next;
      }
      v = next;
   }
}


/**
 * Callback for _mesa_HashWalk.  Free all the shader's program variants
 * which match the given context.
 */
static void
destroy_shader_program_variants_cb(GLuint key, void *data, void *userData)
{
   struct st_context *st = (struct st_context *) userData;
   struct gl_shader *shader = (struct gl_shader *) data;

   switch (shader->Type) {
   case GL_SHADER_PROGRAM_MESA:
      {
         struct gl_shader_program *shProg = (struct gl_shader_program *) data;
         GLuint i;

	 for (i = 0; i < ARRAY_SIZE(shProg->_LinkedShaders); i++) {
	    if (shProg->_LinkedShaders[i])
               destroy_program_variants(st, shProg->_LinkedShaders[i]->Program);
	 }
      }
      break;
   case GL_VERTEX_SHADER:
   case GL_FRAGMENT_SHADER:
   case GL_GEOMETRY_SHADER:
   case GL_TESS_CONTROL_SHADER:
   case GL_TESS_EVALUATION_SHADER:
   case GL_COMPUTE_SHADER:
      break;
   default:
      assert(0);
   }
}


/**
 * Callback for _mesa_HashWalk.  Free all the program variants which match
 * the given context.
 */
static void
destroy_program_variants_cb(GLuint key, void *data, void *userData)
{
   struct st_context *st = (struct st_context *) userData;
   struct gl_program *program = (struct gl_program *) data;
   destroy_program_variants(st, program);
}


/**
 * Walk over all shaders and programs to delete any variants which
 * belong to the given context.
 * This is called during context tear-down.
 */
void
st_destroy_program_variants(struct st_context *st)
{
   /* If shaders can be shared with other contexts, the last context will
    * call DeleteProgram on all shaders, releasing everything.
    */
   if (st->has_shareable_shaders)
      return;

   /* ARB vert/frag program */
   _mesa_HashWalk(st->ctx->Shared->Programs,
                  destroy_program_variants_cb, st);

   /* GLSL vert/frag/geom shaders */
   _mesa_HashWalk(st->ctx->Shared->ShaderObjects,
                  destroy_shader_program_variants_cb, st);
}


/**
 * Compile one shader variant.
 */
static void
st_precompile_shader_variant(struct st_context *st,
                             struct gl_program *prog)
{
   switch (prog->Target) {
   case GL_VERTEX_PROGRAM_ARB: {
      struct st_program *p = (struct st_program *)prog;
      struct st_common_variant_key key;

      memset(&key, 0, sizeof(key));

      key.st = st->has_shareable_shaders ? NULL : st;
      st_get_vp_variant(st, p, &key);
      break;
   }

   case GL_FRAGMENT_PROGRAM_ARB: {
      struct st_program *p = (struct st_program *)prog;
      struct st_fp_variant_key key;

      memset(&key, 0, sizeof(key));

      key.st = st->has_shareable_shaders ? NULL : st;
      st_get_fp_variant(st, p, &key);
      break;
   }

   case GL_TESS_CONTROL_PROGRAM_NV:
   case GL_TESS_EVALUATION_PROGRAM_NV:
   case GL_GEOMETRY_PROGRAM_NV:
   case GL_COMPUTE_PROGRAM_NV: {
      struct st_program *p = st_program(prog);
      struct st_common_variant_key key;

      memset(&key, 0, sizeof(key));

      key.st = st->has_shareable_shaders ? NULL : st;
      st_get_common_variant(st, p, &key);
      break;
   }

   default:
      assert(0);
   }
}

void
st_finalize_program(struct st_context *st, struct gl_program *prog)
{
   if (st->current_program[prog->info.stage] == prog) {
      if (prog->info.stage == MESA_SHADER_VERTEX)
         st->dirty |= ST_NEW_VERTEX_PROGRAM(st, (struct st_program *)prog);
      else
         st->dirty |= ((struct st_program *)prog)->affected_states;
   }

   if (prog->nir)
      nir_sweep(prog->nir);

   /* Create Gallium shaders now instead of on demand. */
   if (ST_DEBUG & DEBUG_PRECOMPILE ||
       st->shader_has_one_variant[prog->info.stage])
      st_precompile_shader_variant(st, prog);
}
