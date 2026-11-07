# lestoroer#local-lights
def can_build(env, platform):
    return not env["disable_3d"] and env["rendering_device"] if "rendering_device" in env else not env["disable_3d"]


def configure(env):
    pass
