# Fusion 360 - Pocket Stand Keychain

Este paquete genera automaticamente el soporte de movil minimalista tipo llavero dentro de Fusion 360.

## Archivos

- `phone_stand_keychain.py`
- `phone_stand_keychain.manifest`

## Como ejecutarlo en Fusion 360 (Windows)

1. Abre Fusion 360.
2. Ve a `Utilities` -> `Scripts and Add-Ins`.
3. En la pestaña `Scripts`, pulsa `+` (Add Script from Folder).
4. Selecciona esta carpeta:
   - `.../3d/fusion360_phone_stand_keychain`
5. Selecciona `phone_stand_keychain` en la lista.
6. Pulsa `Run`.

El script creara un nuevo componente con:

- Cuerpo base redondeado.
- Zona reforzada de llavero con agujero.
- Ranura para apoyar el movil.
- Tope frontal antideslizamiento.
- Dos vaciados de aligerado.

## Parametros editables

Abre `phone_stand_keychain.py` y ajusta estos valores (en mm):

- `body_len`, `body_w`, `body_h`, `corner_r`
- `slot_width`, `slot_depth`, `slot_base_z`
- `lip_h`, `lip_t`
- `ring_hole_d`, `ring_pad_d`, `ring_off_x`
- `light_cut_w`, `light_cut_l`, `light_cut_z`

## Exportar STL

1. En el navegador de Fusion, clic derecho sobre el cuerpo.
2. `Save as Mesh`.
3. Formato STL, refinamiento High.
4. Unidades en mm.
