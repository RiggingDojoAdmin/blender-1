
#include <stdlib.h>

#include "BLI_utildefines.h"

#include "WM_api.h"

#include "ED_lanpr.h"

/* lanpr_cpu.c */
void SCENE_OT_lanpr_calculate_feature_lines(struct wmOperatorType *ot);
void SCENE_OT_lanpr_add_line_layer(struct wmOperatorType *ot);
void SCENE_OT_lanpr_delete_line_layer(struct wmOperatorType *ot);
void SCENE_OT_lanpr_rebuild_all_commands(struct wmOperatorType *ot);
void SCENE_OT_lanpr_auto_create_line_layer(struct wmOperatorType *ot);
void SCENE_OT_lanpr_move_line_layer(struct wmOperatorType *ot);
void SCENE_OT_lanpr_add_line_component(struct wmOperatorType *ot);
void SCENE_OT_lanpr_delete_line_component(struct wmOperatorType *ot);
void SCENE_OT_lanpr_enable_all_line_types(struct wmOperatorType *ot);
void SCENE_OT_lanpr_update_gp_strokes(struct wmOperatorType *ot);
void SCENE_OT_lanpr_bake_gp_strokes(struct wmOperatorType *ot);
void SCENE_OT_lanpr_export_svg(struct wmOperatorType *ot);

void ED_operatortypes_lanpr(void)
{
  /* lanpr:  */
  WM_operatortype_append(SCENE_OT_lanpr_calculate_feature_lines);
  WM_operatortype_append(SCENE_OT_lanpr_add_line_layer);
  WM_operatortype_append(SCENE_OT_lanpr_delete_line_layer);
  WM_operatortype_append(SCENE_OT_lanpr_rebuild_all_commands);
  WM_operatortype_append(SCENE_OT_lanpr_auto_create_line_layer);
  WM_operatortype_append(SCENE_OT_lanpr_move_line_layer);
  WM_operatortype_append(SCENE_OT_lanpr_add_line_component);
  WM_operatortype_append(SCENE_OT_lanpr_delete_line_component);
  WM_operatortype_append(SCENE_OT_lanpr_enable_all_line_types);
  WM_operatortype_append(SCENE_OT_lanpr_update_gp_strokes);
  WM_operatortype_append(SCENE_OT_lanpr_bake_gp_strokes);
  WM_operatortype_append(SCENE_OT_lanpr_export_svg);
}
