/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): Chingiz Dyussenov, Arystanbek Dyussenov, Jan Diederich, Tod Liverseed.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/collada/SceneExporter.cpp
 *  \ingroup collada
 */

extern "C" {
	#include "BLI_utildefines.h"
	#include "BKE_group.h"
	#include "BKE_object.h"
	#include "BLI_listbase.h"
}

#include "SceneExporter.h"
#include "collada_utils.h"

SceneExporter::SceneExporter(COLLADASW::StreamWriter *sw, ArmatureExporter *arm, const ExportSettings *export_settings)
	: COLLADASW::LibraryVisualScenes(sw), arm_exporter(arm), export_settings(export_settings)
{
}

void SceneExporter::exportScene(const EvaluationContext *eval_ctx, Scene *sce)
{
	// <library_visual_scenes> <visual_scene>
	std::string id_naming = id_name(sce);
	openVisualScene(translate_id(id_naming), id_naming);
	exportHierarchy(eval_ctx, sce);
	closeVisualScene();
	closeLibrary();
}

void SceneExporter::exportHierarchy(const EvaluationContext *eval_ctx, Scene *sce)
{	
	LinkNode *node;
	std::vector<Object *> base_objects;

	// Ensure all objects in the export_set are marked
	for (node = this->export_settings->export_set; node; node = node->next) {
		Object *ob = (Object *) node->link;
		ob->id.tag |= LIB_TAG_DOIT;
	}
	
	// Now find all exportable base ojects (highest in export hierarchy)
	for (node = this->export_settings->export_set; node; node = node->next) {
		Object *ob = (Object *) node->link;
		if (bc_is_base_node(this->export_settings->export_set, ob)) {
			switch (ob->type) {
				case OB_MESH:
				case OB_CAMERA:
				case OB_LAMP:
				case OB_EMPTY:
				case OB_ARMATURE:
					base_objects.push_back(ob);
					break;
			}
		}
	}

	// And now export the base objects:
	for (int index = 0; index < base_objects.size(); index++) {
		Object *ob = base_objects[index];
		if (bc_is_marked(ob)) {
			bc_remove_mark(ob);
			writeNodes(eval_ctx, ob, sce);
		}
	}
}


void SceneExporter::writeNodes(const EvaluationContext *eval_ctx, Object *ob, Scene *sce)
{
	// Add associated armature first if available
	bool armature_exported = false;
	Object *ob_arm = bc_get_assigned_armature(ob);
	if (ob_arm != NULL) {
		armature_exported = bc_is_in_Export_set(this->export_settings->export_set, ob_arm);
		if (armature_exported && bc_is_marked(ob_arm)) {
			bc_remove_mark(ob_arm);
			writeNodes(eval_ctx, ob_arm, sce);
			armature_exported = true;
		}
	}

	COLLADASW::Node colladaNode(mSW);
	colladaNode.setNodeId(translate_id(id_name(ob)));
	colladaNode.setNodeName(translate_id(id_name(ob)));
	colladaNode.setType(COLLADASW::Node::NODE);

	colladaNode.start();

	std::list<Object *> child_objects;

	// list child objects
	LinkNode *node;
	for (node=this->export_settings->export_set; node; node=node->next) {
		// cob - child object
		Object *cob = (Object *)node->link;

		if (cob->parent == ob) {
			switch (cob->type) {
				case OB_MESH:
				case OB_CAMERA:
				case OB_LAMP:
				case OB_EMPTY:
				case OB_ARMATURE:
					if (bc_is_marked(cob))
						child_objects.push_back(cob);
					break;
			}
		}
	}

	if (ob->type == OB_MESH && armature_exported)
		// for skinned mesh we write obmat in <bind_shape_matrix>
		TransformWriter::add_node_transform_identity(colladaNode);
	else {
		TransformWriter::add_node_transform_ob(colladaNode, ob, this->export_settings->export_transformation_type);
	}

	// <instance_geometry>
	if (ob->type == OB_MESH) {
		bool instance_controller_created = false;
		if (armature_exported) {
			instance_controller_created = arm_exporter->add_instance_controller(ob);
		}
		if (!instance_controller_created) {
			COLLADASW::InstanceGeometry instGeom(mSW);
			instGeom.setUrl(COLLADASW::URI(COLLADABU::Utils::EMPTY_STRING, get_geometry_id(ob, this->export_settings->use_object_instantiation)));
			instGeom.setName(translate_id(id_name(ob)));
			InstanceWriter::add_material_bindings(instGeom.getBindMaterial(), ob, this->export_settings->active_uv_only);

			instGeom.add();
		}
	}

	// <instance_controller>
	else if (ob->type == OB_ARMATURE) {
		arm_exporter->add_armature_bones(eval_ctx, ob, sce, this, child_objects);
	}

	// <instance_camera>
	else if (ob->type == OB_CAMERA) {
		COLLADASW::InstanceCamera instCam(mSW, COLLADASW::URI(COLLADABU::Utils::EMPTY_STRING, get_camera_id(ob)));
		instCam.add();
	}

	// <instance_light>
	else if (ob->type == OB_LAMP) {
		COLLADASW::InstanceLight instLa(mSW, COLLADASW::URI(COLLADABU::Utils::EMPTY_STRING, get_light_id(ob)));
		instLa.add();
	}

	// empty object
	else if (ob->type == OB_EMPTY) { // TODO: handle groups (OB_DUPLIGROUP
		if ((ob->transflag & OB_DUPLIGROUP) == OB_DUPLIGROUP && ob->dup_group) {
			Group *group = ob->dup_group;
			/* printf("group detected '%s'\n", group->id.name + 2); */
			FOREACH_GROUP_OBJECT_BEGIN(group, object)
			{
				printf("\t%s\n", object->id.name);
			}
			FOREACH_GROUP_OBJECT_END;
		}
	}

	if (ob->type == OB_ARMATURE) {
		colladaNode.end();
	}

	if (BLI_listbase_is_empty(&ob->constraints) == false) {
		bConstraint *con = (bConstraint *) ob->constraints.first;
		while (con) {
			std::string con_name(translate_id(con->name));
			std::string con_tag = con_name + "_constraint";
			printf("%s\n", con_name.c_str());
			printf("%s\n\n", con_tag.c_str());
			colladaNode.addExtraTechniqueChildParameter("blender",con_tag,"type",con->type);
			colladaNode.addExtraTechniqueChildParameter("blender",con_tag,"enforce",con->enforce);
			colladaNode.addExtraTechniqueChildParameter("blender",con_tag,"flag",con->flag);
			colladaNode.addExtraTechniqueChildParameter("blender",con_tag,"headtail",con->headtail);
			colladaNode.addExtraTechniqueChildParameter("blender",con_tag,"lin_error",con->lin_error);
			colladaNode.addExtraTechniqueChildParameter("blender",con_tag,"own_space",con->ownspace);
			colladaNode.addExtraTechniqueChildParameter("blender",con_tag,"rot_error",con->rot_error);
			colladaNode.addExtraTechniqueChildParameter("blender",con_tag,"tar_space",con->tarspace);
			colladaNode.addExtraTechniqueChildParameter("blender",con_tag,"lin_error",con->lin_error);
			
			//not ideal: add the target object name as another parameter. 
			//No real mapping in the .dae
			//Need support for multiple target objects also.
			const bConstraintTypeInfo *cti = BKE_constraint_typeinfo_get(con);
			ListBase targets = {NULL, NULL};
			if (cti && cti->get_constraint_targets) {
			
				bConstraintTarget *ct;
				Object *obtar;
			
				cti->get_constraint_targets(con, &targets);

				for (ct = (bConstraintTarget *)targets.first; ct; ct = ct->next) {
					obtar = ct->tar;
					std::string tar_id((obtar) ? id_name(obtar) : "");
					colladaNode.addExtraTechniqueChildParameter("blender",con_tag,"target_id",tar_id);
				}

				if (cti->flush_constraint_targets)
					cti->flush_constraint_targets(con, &targets, 1);

			}

			con = con->next;
		}
	}

	for (std::list<Object *>::iterator i = child_objects.begin(); i != child_objects.end(); ++i) {
		if (bc_is_marked(*i)) {
			bc_remove_mark(*i);
			writeNodes(eval_ctx, *i, sce);
		}
	}

	if (ob->type != OB_ARMATURE)
		colladaNode.end();
}

