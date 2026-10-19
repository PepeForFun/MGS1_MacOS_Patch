# Build: python3 -I tools/recomp.py <game>/mgsi.exe build/gen && make -j10
#        python3 -I tools/recomp.py <game>/mgsvr.exe build/gen-vr && make -j10 GEN=build/gen-vr BIN=build/mgsvr
CC = clang
GEN ?= build/gen
BIN ?= build/mgs
OBJ = build/obj
GOBJ = $(OBJ)/$(notdir $(GEN))
CFLAGS = -O2 -g -Iruntime -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable \
	-Wno-parentheses-equality -Wno-unused-value
RT_C = $(wildcard runtime/*.c)
RT_M = $(wildcard runtime/*.m)
RT_OBJ = $(patsubst runtime/%.c,$(OBJ)/rt_%.o,$(RT_C)) $(patsubst runtime/%.m,$(OBJ)/rtm_%.o,$(RT_M))
GEN_OBJ = $(patsubst $(GEN)/%.c,$(GOBJ)/%.o,$(wildcard $(GEN)/gen_*.c))
FRAMEWORKS = -framework Cocoa -framework Metal -framework QuartzCore -framework AudioToolbox \
	-framework GameController -framework CoreText -framework CoreGraphics -framework ImageIO \
	-framework CoreServices

$(BIN): $(GEN_OBJ) $(RT_OBJ)
	$(CC) -o $@ $^ $(FRAMEWORKS)

$(GOBJ)/%.o: $(GEN)/%.c runtime/recomp.h | $(GOBJ)
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ)/rt_%.o: runtime/%.c runtime/*.h | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ)/rtm_%.o: runtime/%.m runtime/*.h | $(OBJ)
	$(CC) $(CFLAGS) -fobjc-arc -c $< -o $@
$(OBJ) $(GOBJ):
	mkdir -p $@
clean:
	rm -rf $(OBJ) build/mgs build/mgsvr
