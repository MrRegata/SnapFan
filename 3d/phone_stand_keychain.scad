// Pocket Stand Keychain - Minimal
// Parametric OpenSCAD design focused on portability, printability and clean style.

$fn = 72;

// ---------- Main parameters ----------
body_len    = 70;
body_w      = 28;
body_h      = 5.2;
corner_r    = 4.2;

slot_width  = 12.8; // fit for most phones with slim-medium cases
slot_depth  = 18.0;
slot_angle  = 68;   // visual angle balance (portrait + landscape)
slot_base_z = 1.1;  // keeps enough material under slot

lip_h       = 2.2;  // front anti-slip lip height
lip_t       = 1.6;

ring_hole_d = 5.0;
ring_pad_d  = 11.5;
ring_off_x  = 8.0;

light_cut_w = 8.0;
light_cut_l = 18.0;
light_cut_z = 2.2;

// ---------- Helpers ----------
module rounded_block(len, wid, hei, rad) {
    hull() {
        for (x = [rad, len - rad])
            for (y = [rad, wid - rad])
                translate([x, y, 0]) cylinder(h = hei, r = rad);
    }
}

module slot_cutter() {
    // Rotated rectangular prism to create the support slot.
    translate([body_len * 0.53, body_w / 2, slot_base_z])
        rotate([0, -slot_angle, 0])
            translate([-slot_depth / 2, -slot_width / 2, -body_h])
                cube([slot_depth, slot_width, body_h * 3]);
}

module front_lip() {
    // Front lip stops the phone from sliding down.
    translate([body_len * 0.73, (body_w - slot_width) / 2, body_h - lip_h])
        cube([lip_t, slot_width, lip_h]);
}

module lightening_cuts() {
    // Two internal cuts keep the look minimal and reduce weight.
    for (x = [body_len * 0.24, body_len * 0.41])
        translate([x, (body_w - light_cut_w) / 2, body_h - light_cut_z])
            rounded_block(light_cut_l, light_cut_w, light_cut_z + 0.1, 1.2);
}

module keyring_zone() {
    // Reinforced keyring mount integrated into the body silhouette.
    translate([ring_off_x, body_w / 2, 0])
        difference() {
            cylinder(h = body_h, d = ring_pad_d);
            translate([0, 0, -0.1]) cylinder(h = body_h + 0.2, d = ring_hole_d);
        }
}

// ---------- Final model ----------
module model() {
    difference() {
        union() {
            rounded_block(body_len, body_w, body_h, corner_r);
            keyring_zone();
            front_lip();
        }

        slot_cutter();
        lightening_cuts();

        // Palm comfort chamfer near tail (simple angled cut).
        translate([body_len - 9, -1, body_h - 0.2])
            rotate([0, 30, 0])
                cube([18, body_w + 2, body_h * 2]);
    }
}

model();
