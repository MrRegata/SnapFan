// ArcDock Key V2
// Minimal keychain phone stand with refined silhouette and better visual balance.

$fn = 96;

// ---------- Parameters (mm) ----------
body_len      = 82;
body_w        = 24;
body_h        = 5.0;

ring_outer_d  = 14;
ring_hole_d   = 5.0;
ring_x        = 9;

slot_w        = 13.2; // smartphone + most slim/medium cases
slot_d        = 20;
slot_angle    = 67;
slot_floor    = 1.2;

lip_h         = 2.4;
lip_t         = 1.7;

waist_cut_d   = 38;   // soft hourglass waist for aesthetics + grip
waist_depth   = 0.9;

window_l      = 28;   // visual lightness window
window_w      = 8.2;
window_z      = 2.1;

// ---------- Helpers ----------
module capsule2d(len, wid) {
    hull() {
        translate([wid / 2, wid / 2]) circle(d = wid);
        translate([len - wid / 2, wid / 2]) circle(d = wid);
    }
}

module rounded_block(len, wid, hei, rad) {
    hull() {
        for (x = [rad, len - rad])
            for (y = [rad, wid - rad])
                translate([x, y, 0]) cylinder(h = hei, r = rad);
    }
}

module base_body() {
    linear_extrude(height = body_h)
        capsule2d(body_len, body_w);
}

module keyring_pad() {
    translate([ring_x, body_w / 2, 0])
        difference() {
            cylinder(h = body_h, d = ring_outer_d);
            translate([0, 0, -0.2]) cylinder(h = body_h + 0.4, d = ring_hole_d);
        }
}

module phone_slot_cut() {
    // Main slot cut, tilted for natural viewing angle.
    translate([body_len * 0.58, body_w / 2, slot_floor])
        rotate([0, -slot_angle, 0])
            translate([-slot_d / 2, -slot_w / 2, -body_h * 1.2])
                rounded_block(slot_d, slot_w, body_h * 3, 1.2);
}

module comfort_top_sweep() {
    // Subtle top sweep at tail for a cleaner profile.
    translate([body_len - 12, -2, body_h - 0.2])
        rotate([0, 28, 0])
            cube([22, body_w + 4, body_h * 2]);
}

module waist_reliefs() {
    // Gentle side reliefs to avoid a plain rectangular look.
    for (y = [-waist_cut_d / 2, waist_cut_d / 2])
        translate([body_len * 0.52, body_w / 2 + y, body_h - waist_depth])
            rotate([0, 0, 90])
                scale([1.0, 0.55, 1])
                    cylinder(h = waist_depth + 0.2, d = waist_cut_d);
}

module center_window() {
    // Window gives lightness and a stronger premium aesthetic.
    translate([body_len * 0.27, (body_w - window_w) / 2, body_h - window_z])
        rounded_block(window_l, window_w, window_z + 0.2, 1.4);
}

module front_lip() {
    // Anti-slip lip where phone rests.
    translate([body_len * 0.76, (body_w - slot_w) / 2, body_h - lip_h])
        rounded_block(lip_t, slot_w, lip_h, 0.5);
}

// ---------- Final ----------
module model() {
    difference() {
        union() {
            base_body();
            keyring_pad();
            front_lip();
        }

        phone_slot_cut();
        center_window();
        comfort_top_sweep();
        waist_reliefs();
    }
}

model();
