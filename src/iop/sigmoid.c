/*
    This file is part of darktable,
    Copyright (C) 2010-2020 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
// our includes go first:
#include "bauhaus/bauhaus.h"
#include "common/rgb_norms.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <stdlib.h>

// To have your module compile and appear in darkroom, add it to CMakeLists.txt, with
//  add_iop(sigmoid "sigmoid.c")
// and to iop_order.c, in the initialisation of legacy_order & v30_order with:
//  { {XX.0f }, "sigmoid", 0},

// Module version number
DT_MODULE_INTROSPECTION(1, dt_iop_sigmoid_params_t)


// Enums used in params_t can have $DESCRIPTIONs that will be used to
// automatically populate a combobox with dt_bauhaus_combobox_from_params.
// They are also used in the history changes tooltip.
// Combobox options will be presented in the same order as defined here.
// These numbers must not be changed when a new version is introduced.
typedef enum dt_iop_sigmoid_type_t
{
  DT_SIGMOID_LOGLOGISTIC = 0,  // $DESCRIPTION: "Log-Logisitic"
  DT_SIGMOID_WEIBULL = 1,      // $DESCRIPTION: "Weibull"
} dt_iop_sigmoid_type_t;

#define MIDDLE_GREY 0.18f

typedef struct dt_iop_sigmoid_params_t
{
  // The parameters defined here fully record the state of the module and are stored
  // (as a serialized binary blob) into the db.
  // Make sure everything is in here does not depend on temporary memory (pointers etc).
  // This struct defines the layout of self->params and self->default_params.
  // You should keep changes to this struct to a minimum.
  // If you have to change this struct, it will break
  // user data bases, and you have to increment the version
  // of DT_MODULE_INTROSPECTION(VERSION) above and provide a legacy_params upgrade path!
  //
  // Tags in the comments get picked up by the introspection framework and are
  // used in gui_init to set range and labels (for widgets and history)
  // and value checks before commit_params.
  // If no explicit init() is specified, the default implementation uses $DEFAULT tags
  // to initialise self->default_params, which is then used in gui_init to set widget defaults.

  float middle_grey_contrast;  // $MIN: 0.1 $MAX: 10.0 $DEFAULT: 1.4 $DESCRIPTION: "Contrast"
  dt_iop_sigmoid_type_t cumulative_distribution;  // $DEFAULT: DT_SIGMOID_LOGLOGISTIC $DESCRIPTION: "Curve type"
} dt_iop_sigmoid_params_t;

typedef struct dt_iop_sigmoid_global_data_t
{} dt_iop_sigmoid_global_data_t;

typedef struct dt_iop_sigmoid_gui_data_t
{
  // Whatever you need to make your gui happy and provide access to widgets between gui_init, gui_update etc.
  // Stored in self->gui_data while in darkroom.
  // To permanently store per-user gui configuration settings, you could use dt_conf_set/_get.
  GtkWidget *contrast_slider, *distribution_list;
} dt_iop_sigmoid_gui_data_t;


// this returns a translatable name
const char *name()
{
  // make sure you put all your translatable strings into _() !
  return _("sigmoid");
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING;
}

// where does it appear in the gui?
int default_group()
{
  return IOP_GROUP_TONE | IOP_GROUP_TECHNICAL;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}

// Whenever new fields are added to (or removed from) dt_iop_..._params_t or when their meaning
// changes, a translation from the old to the new version must be added here.
// A verbatim copy of the old struct definition should be copied into the routine with a _v?_t ending.
// Since this will get very little future testing (because few developers still have very
// old versions lying around) existing code should be changed as little as possible, if at all.
//
// Upgrading from an older version than the previous one should always go through all in between versions
// (unless there was a bug) so that the end result will always be the same.
//
// Be careful with changes to structs that are included in _params_t
//
// Individually copy each existing field that is still in the new version. This is robust even if reordered.
// If only new fields were added at the end, one call can be used:
//   memcpy(n, o, sizeof *o);
//
// Hardcode the default values for new fields that were added, rather than referring to default_params;
// in future, the field may not exist anymore or the default may change. The best default for a new version
// to replicate a previous version might not be the optimal default for a fresh image.
//
// FIXME: the calling logic needs to be improved to call upgrades from consecutive version in sequence.
int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  return 1;
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, p1, self->params_size);
}


static inline float rgb_luma(const float pixel[4], const dt_iop_order_iccprofile_info_t *const work_profile)
{
  return (work_profile)
            ? dt_workprofile_rgb_luminance(pixel, work_profile->matrix_in)
            : dt_camera_rgb_luminance(pixel);
}

void process_loglogistic(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                         void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_sigmoid_params_t *d = (dt_iop_sigmoid_params_t *)piece->data;
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);

  const float *const in = (const float *)ivoid;
  float *const out = (float *)ovoid;
  const size_t npixels = (size_t)roi_in->width * roi_in->height;

  const float power = d->middle_grey_contrast;
  const float pivot = (1.0f - MIDDLE_GREY) / MIDDLE_GREY;
  const float grey = MIDDLE_GREY;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(npixels, work_profile, pivot, grey, power) \
  dt_omp_sharedconst(in, out) \
  schedule(static)
#endif
  for(size_t k = 0; k < 4*npixels; k += 4)
  {
    // Desature a bit to get proper roll off to white in highlights
    // This is taken from the ACES RRT implementation
    const float luma = rgb_luma(in + k, work_profile);
    for(size_t c = 0; c < 3; c++)
    {
      const float desat = fmaxf(luma + 0.96f * (in[k + c] - luma), 0.0f);
      out[k + c] = 1.0f - pivot / (pivot + powf(desat / grey, power));
    }
    // Copy over the alpha channel
    out[k + 3] = in[k + 3];
  }
}

void process_weibull(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                         void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_sigmoid_params_t *d = (dt_iop_sigmoid_params_t *)piece->data;
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);

  const float *const in = (const float *)ivoid;
  float *const out = (float *)ovoid;
  const size_t npixels = (size_t)roi_in->width * roi_in->height;

  const float power = 0.91f * d->middle_grey_contrast;
  const float scale = MIDDLE_GREY / powf(-logf(1.0f - MIDDLE_GREY), 1.0f / power);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(npixels, work_profile, scale, power) \
  dt_omp_sharedconst(in, out) \
  schedule(static)
#endif
  for(size_t k = 0; k < 4*npixels; k += 4)
  {
    // Desature a bit to get proper roll off to white in highlights
    // This is taken from the ACES RRT implementation
    const float luma = rgb_luma(in + k, work_profile);
    for(size_t c = 0; c < 3; c++)
    {
      const float desat = fmaxf(luma + 0.96f * (in[k + c] - luma), 0.0f);
      out[k + c] = 1.0f - expf(-powf(desat / scale, power));
    }
    // Copy over the alpha channel
    out[k + 3] = in[k + 3];
  }
}

/** process, all real work is done here. */
void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  // this is called for preview and full pipe separately, each with its own pixelpipe piece.
  // get our data struct:
  dt_iop_sigmoid_params_t *d = (dt_iop_sigmoid_params_t *)piece->data;
  const dt_iop_sigmoid_type_t cdf_type = d->cumulative_distribution;

  if (cdf_type == DT_SIGMOID_LOGLOGISTIC)
  {
    process_loglogistic(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
  else
  {
    process_weibull(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
}

void init_global(dt_iop_module_so_t *module)
{
  module->data = malloc(sizeof(dt_iop_sigmoid_global_data_t));
}

void cleanup(dt_iop_module_t *module)
{
  // Releases any memory allocated in init(module)
  // Implement this function explicitly if the module allocates additional memory besides (default_)params.
  // this is rare.
  free(module->params);
  module->params = NULL;
  free(module->default_params);
  module->default_params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  free(module->data);
  module->data = NULL;
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_sigmoid_gui_data_t *g = (dt_iop_sigmoid_gui_data_t *)self->gui_data;
  dt_iop_sigmoid_params_t *p = (dt_iop_sigmoid_params_t *)self->params;

  dt_bauhaus_slider_set(g->contrast_slider, p->middle_grey_contrast);
  dt_bauhaus_combobox_set_from_value(g->distribution_list, p->cumulative_distribution);
  gui_changed(self, NULL, NULL);
}

void gui_init(dt_iop_module_t *self)
{
  // Allocates memory for the module's user interface in the darkroom and
  // sets up the widgets in it.
  //
  // self->widget needs to be set to the top level widget.
  // This can be a (vertical) box, a grid or even a notebook. Modules that are
  // disabled for certain types of images (for example non-raw) may use a stack
  // where one of the pages contains just a label explaining why it is disabled.
  //
  // Widgets that are directly linked to a field in params_t may be set up using the
  // dt_bauhaus_..._from_params family. They take a string with the field
  // name in the params_t struct definition. The $MIN, $MAX and $DEFAULT tags will be
  // used to set up the widget (slider) ranges and default values and the $DESCRIPTION
  // is used as the widget label.
  //
  // The _from_params calls also set up an automatic callback that updates the field in params
  // whenever the widget is changed. In addition, gui_changed is called, if it exists,
  // so that any other required changes, to dependent fields or to gui widgets, can be made.
  //
  // Whenever self->params changes (switching images or history) the widget values have to
  // be updated in gui_update.
  //
  // Do not set the value of widgets or configure them depending on field values here;
  // this should be done in gui_update (or gui_changed or individual widget callbacks)
  //
  // If any default values for (slider) widgets or options (in comboboxes) depend on the
  // type of image, then the widgets have to be updated in reload_params.
  dt_iop_sigmoid_gui_data_t *g = IOP_GUI_ALLOC(sigmoid);
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->contrast_slider = dt_bauhaus_slider_from_params(self, "middle_grey_contrast");
  g->distribution_list = dt_bauhaus_combobox_from_params(self, "cumulative_distribution");
}

void gui_cleanup(dt_iop_module_t *self)
{
  // This only needs to be provided if gui_init allocates any memory or resources besides
  // self->widget and gui_data_t. The default function (if an explicit one isn't provided here)
  // takes care of gui_data_t (and gtk destroys the widget anyway). If you override the default,
  // you have to do whatever you have to do, and also call IOP_GUI_FREE to clean up gui_data_t.

  IOP_GUI_FREE;
}

/** additional, optional callbacks to capture darkroom center events. */
// void gui_post_expose(dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx,
// int32_t pointery);
// int mouse_moved(dt_iop_module_t *self, double x, double y, double pressure, int which);
// int button_pressed(dt_iop_module_t *self, double x, double y, double pressure, int which, int type,
// uint32_t state);
// int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state);
// int scrolled(dt_iop_module_t *self, double x, double y, int up, uint32_t state);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
