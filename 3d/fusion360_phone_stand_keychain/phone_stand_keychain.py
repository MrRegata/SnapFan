# Author: GitHub Copilot
# Description: Creates a minimal phone stand keychain in Fusion 360.

import adsk.core
import adsk.fusion
import traceback


def mm(value_mm: float) -> float:
    # Fusion 360 internal length unit is cm.
    return value_mm / 10.0


def top_face_of(body: adsk.fusion.BRepBody):
    top = None
    max_z = -1e9
    for face in body.faces:
        geom = face.geometry
        if isinstance(geom, adsk.core.Plane):
            z = geom.origin.z
            normal = geom.normal
            if normal.z > 0.99 and z > max_z:
                max_z = z
                top = face
    return top


def set_cut_extent(extrude_input: adsk.fusion.ExtrudeFeatureInput, depth_mm: float, use_negative: bool = True):
    # Top-face sketches may need explicit direction to cut into the body.
    depth_value = adsk.core.ValueInput.createByReal(mm(depth_mm))
    depth_extent = adsk.fusion.DistanceExtentDefinition.create(depth_value)
    direction = adsk.fusion.ExtentDirections.NegativeExtentDirection if use_negative else adsk.fusion.ExtentDirections.PositiveExtentDirection
    extrude_input.setOneSideExtent(depth_extent, direction)


def profile_near_point(sketch: adsk.fusion.Sketch, x_cm: float, y_cm: float):
    best = None
    best_score = 1e18
    for i in range(sketch.profiles.count):
        p = sketch.profiles.item(i)
        bb = p.boundingBox
        if bb.minPoint.x <= x_cm <= bb.maxPoint.x and bb.minPoint.y <= y_cm <= bb.maxPoint.y:
            cx = (bb.minPoint.x + bb.maxPoint.x) * 0.5
            cy = (bb.minPoint.y + bb.maxPoint.y) * 0.5
            score = abs(cx - x_cm) + abs(cy - y_cm)
            if score < best_score:
                best_score = score
                best = p
    return best


def add_cut_with_retry(extrudes: adsk.fusion.ExtrudeFeatures,
                       profile: adsk.fusion.Profile,
                       depth_mm: float,
                       target_body: adsk.fusion.BRepBody):
    # Retry with opposite direction to handle API orientation differences.
    for use_negative in (True, False):
        cut_input = extrudes.createInput(profile, adsk.fusion.FeatureOperations.CutFeatureOperation)
        set_cut_extent(cut_input, depth_mm, use_negative)
        # Some Fusion builds require a Python list for participantBodies.
        try:
            cut_input.participantBodies = [target_body]
        except TypeError:
            # Fallback: skip explicit participant bodies if API binding differs.
            pass
        try:
            extrudes.add(cut_input)
            return
        except RuntimeError:
            pass
    raise RuntimeError('No se pudo realizar el corte en ninguna direccion.')


def run(context):
    ui = None
    try:
        app = adsk.core.Application.get()
        ui = app.userInterface
        design = adsk.fusion.Design.cast(app.activeProduct)
        if not design:
            ui.messageBox('Abre un diseno de Fusion 360 antes de ejecutar el script.')
            return

        # ---------- Parameters (mm) ----------
        body_len = 70.0
        body_w = 28.0
        body_h = 5.2
        corner_r = 4.2

        slot_width = 12.8
        slot_depth = 18.0
        slot_base_z = 1.1

        lip_h = 2.2
        lip_t = 1.6

        ring_hole_d = 5.0
        ring_pad_d = 11.5
        ring_off_x = 8.0

        light_cut_w = 8.0
        light_cut_l = 18.0
        light_cut_z = 2.2

        root = design.rootComponent
        occ = root.occurrences.addNewComponent(adsk.core.Matrix3D.create())
        comp = occ.component
        comp.name = 'PocketStandKeychain'

        # ---------- Base body ----------
        sketch = comp.sketches.add(comp.xYConstructionPlane)
        lines = sketch.sketchCurves.sketchLines
        p1 = adsk.core.Point3D.create(0, 0, 0)
        p2 = adsk.core.Point3D.create(mm(body_len), mm(body_w), 0)
        lines.addTwoPointRectangle(p1, p2)

        prof = sketch.profiles.item(0)
        extrudes = comp.features.extrudeFeatures
        base_input = extrudes.createInput(prof, adsk.fusion.FeatureOperations.NewBodyFeatureOperation)
        base_input.setDistanceExtent(False, adsk.core.ValueInput.createByReal(mm(body_h)))
        base_ext = extrudes.add(base_input)
        body = base_ext.bodies.item(0)

        # Round vertical corners.
        edge_set = adsk.core.ObjectCollection.create()
        for e in body.edges:
            sv = e.startVertex.geometry
            ev = e.endVertex.geometry
            if abs(sv.x - ev.x) < 1e-6 and abs(sv.y - ev.y) < 1e-6 and abs(sv.z - ev.z) > 1e-6:
                edge_set.add(e)

        if edge_set.count > 0:
            fillet_feats = comp.features.filletFeatures
            fillet_input = fillet_feats.createInput()
            fillet_input.addConstantRadiusEdgeSet(
                edge_set,
                adsk.core.ValueInput.createByReal(mm(corner_r)),
                True
            )
            fillet_feats.add(fillet_input)

        # ---------- Keyring reinforced pad ----------
        sketch_ring = comp.sketches.add(comp.xYConstructionPlane)
        circles = sketch_ring.sketchCurves.sketchCircles
        ring_center = adsk.core.Point3D.create(mm(ring_off_x), mm(body_w / 2.0), 0)
        circles.addByCenterRadius(ring_center, mm(ring_pad_d / 2.0))

        ring_prof = sketch_ring.profiles.item(0)
        ring_input = extrudes.createInput(ring_prof, adsk.fusion.FeatureOperations.JoinFeatureOperation)
        ring_input.setDistanceExtent(False, adsk.core.ValueInput.createByReal(mm(body_h)))
        extrudes.add(ring_input)

        # Update body reference after join.
        body = comp.bRepBodies.item(0)

        # Through-hole for keyring.
        sketch_hole = comp.sketches.add(comp.xYConstructionPlane)
        hole_center = adsk.core.Point3D.create(mm(ring_off_x), mm(body_w / 2.0), 0)
        sketch_hole.sketchCurves.sketchCircles.addByCenterRadius(hole_center, mm(ring_hole_d / 2.0))
        hole_prof = sketch_hole.profiles.item(0)

        hole_input = extrudes.createInput(hole_prof, adsk.fusion.FeatureOperations.CutFeatureOperation)
        hole_input.setDistanceExtent(False, adsk.core.ValueInput.createByReal(mm(body_h + 2.0)))
        extrudes.add(hole_input)

        # ---------- Top operations ----------
        body = comp.bRepBodies.item(0)
        top_face = top_face_of(body)
        if not top_face:
            ui.messageBox('No se encontro la cara superior para continuar.')
            return

        # Phone slot pocket (top cut).
        sketch_slot = comp.sketches.add(top_face)
        slot_lines = sketch_slot.sketchCurves.sketchLines
        sx0 = mm(body_len * 0.53 - slot_depth / 2.0)
        sx1 = mm(body_len * 0.53 + slot_depth / 2.0)
        sy0 = mm((body_w - slot_width) / 2.0)
        sy1 = mm((body_w + slot_width) / 2.0)
        slot_lines.addTwoPointRectangle(
            adsk.core.Point3D.create(sx0, sy0, 0),
            adsk.core.Point3D.create(sx1, sy1, 0)
        )
        slot_center_x = mm(body_len * 0.53)
        slot_center_y = mm(body_w / 2.0)
        slot_prof = profile_near_point(sketch_slot, slot_center_x, slot_center_y)
        if not slot_prof:
            ui.messageBox('No se encontro el perfil de la ranura del movil.')
            return

        slot_depth_cut = max(0.8, body_h - slot_base_z)
        body = comp.bRepBodies.item(0)
        add_cut_with_retry(extrudes, slot_prof, slot_depth_cut, body)

        # Front anti-slip lip.
        body = comp.bRepBodies.item(0)
        top_face = top_face_of(body)
        sketch_lip = comp.sketches.add(top_face)
        lip_lines = sketch_lip.sketchCurves.sketchLines
        lx0 = mm(body_len * 0.73)
        lx1 = mm(body_len * 0.73 + lip_t)
        ly0 = mm((body_w - slot_width) / 2.0)
        ly1 = mm((body_w + slot_width) / 2.0)
        lip_lines.addTwoPointRectangle(
            adsk.core.Point3D.create(lx0, ly0, 0),
            adsk.core.Point3D.create(lx1, ly1, 0)
        )

        if sketch_lip.profiles.count > 0:
            lip_prof = sketch_lip.profiles.item(0)
            lip_input = extrudes.createInput(lip_prof, adsk.fusion.FeatureOperations.JoinFeatureOperation)
            lip_input.setDistanceExtent(False, adsk.core.ValueInput.createByReal(mm(lip_h)))
            extrudes.add(lip_input)

        # Lightening cuts.
        body = comp.bRepBodies.item(0)
        top_face = top_face_of(body)
        sketch_cut = comp.sketches.add(top_face)
        cut_lines = sketch_cut.sketchCurves.sketchLines

        for x0_mm in (body_len * 0.24, body_len * 0.41):
            x0 = mm(x0_mm)
            x1 = mm(x0_mm + light_cut_l)
            y0 = mm((body_w - light_cut_w) / 2.0)
            y1 = mm((body_w + light_cut_w) / 2.0)
            cut_lines.addTwoPointRectangle(
                adsk.core.Point3D.create(x0, y0, 0),
                adsk.core.Point3D.create(x1, y1, 0)
            )

        body = comp.bRepBodies.item(0)
        for i in range(sketch_cut.profiles.count):
            p = sketch_cut.profiles.item(i)
            try:
                add_cut_with_retry(extrudes, p, light_cut_z, body)
            except RuntimeError:
                # Skip non-target profiles created by face segmentation.
                continue

        ui.messageBox('Modelo creado: PocketStandKeychain')

    except:
        if ui:
            ui.messageBox('Fallo:\n{}'.format(traceback.format_exc()))
