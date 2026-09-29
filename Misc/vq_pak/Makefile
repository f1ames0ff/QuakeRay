INPUT := gfx/conback.lmp \
	gfx/crosshair-000.png \
	gfx/crosshair-001.png \
	gfx/crosshair-020.png \
	gfx/crosshair-021.png \
	gfx/crosshair-148.png \
	gfx/mainmenu2.lmp \
	gfx/p_mods.lmp \
	maps/e1m1@c49d.ent \
	maps/e1m2@0caa.ent \
	maps/e1m4@958e.ent \
	maps/e2m2@fbfe.ent \
	maps/e2m3@237a.ent \
	maps/e2m7@10a8.ent \
	default.cfg

OUTPUT := vkquake.pak

$(OUTPUT): $(INPUT)
	./mkpak.sh $(INPUT) > $(OUTPUT)

.PHONY: clean
clean:
	rm -f $(OUTPUT)

