def can_build(env, platform):
    # Editor and platforms where execute_with_pipe are available only.
    return env.editor_build and env["platform"] in ["android", "linuxbsd", "macos", "windows"]


def configure(env):
    pass
