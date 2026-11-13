-- @envy bin "tools"
-- Middle link of the nested fixture: its own entry anchors here, sub's keep anchoring
-- on sub, and each file sees its own envy.PROJECT_ROOT.

local sub = envy.subproject("../sub")
PACKAGES = envy.extend({ { spec = "nested.tool@r1", source = "nested_spec.lua" } },
                       sub.PACKAGES)

ROOT_SEEN = envy.PROJECT_ROOT
SUB_ROOT_SEEN = sub.ROOT_SEEN
SUB_FIELD = sub.PROJECT_ROOT
