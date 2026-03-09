apps = [
    "print_backtrace", "errorline", "0", "1", "sum_sequence", "singlepageheap",
    "alloc0", "alloc1", "wait", "semaphore", "cow", "relativepath", "exec"
]

out = "\n# -------- CHALLENGE APPS --------\n"

targets = []

for app in apps:
    app_id = "USER_" + app.upper()
    prefix = app
    if app == "0":
        prefix = "app0"
        app_id = "USER_APP0"
    elif app == "1":
        prefix = "app1"
        app_id = "USER_APP1"
    else:
        prefix = "app_" + app
        
    out += f"{app_id}_CPPS := user/{prefix}.c user/user_lib.c\n"
    out += f"{app_id}_OBJS := $(addprefix $(OBJ_DIR)/, $(patsubst %.c,%.o,$({app_id}_CPPS)))\n"
    out += f"{app_id}_TARGET := $(HOSTFS_ROOT)/bin/{prefix}\n\n"
    targets.append(f"$({app_id}_TARGET)")

out += "CHAL_TARGETS := " + " ".join(targets) + "\n\n"

for app in apps:
    app_id = "USER_" + app.upper()
    if app == "0": app_id = "USER_APP0"
    if app == "1": app_id = "USER_APP1"
    
    out += f"$({app_id}_TARGET): $(OBJ_DIR) $(UTIL_LIB) $({app_id}_OBJS)\n"
    out += f"\t@echo \"linking\" $@ ...\n"
    out += f"\t-@mkdir -p $(HOSTFS_ROOT)/bin\n"
    out += f"\t@$(COMPILE) --entry=main $({app_id}_OBJS) $(UTIL_LIB) -o $@\n\n"

with open("Makefile", "a") as f:
    f.write(out)

with open("Makefile", "r") as f:
    content = f.read()

content = content.replace("all: $(KERNEL_TARGET)", "all: $(KERNEL_TARGET) $(CHAL_TARGETS)")
content = content.replace("run: $(KERNEL_TARGET)", "run: $(KERNEL_TARGET) $(CHAL_TARGETS)")
content = content.replace("spike $(KERNEL_TARGET) /bin/app_shell", "spike -p2 $(KERNEL_TARGET) /bin/app_shell")

with open("Makefile", "w") as f:
    f.write(content)
