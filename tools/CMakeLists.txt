add_executable(cooker cooker/Main.cpp)
# world + narrative pull every form family the cooker must know (audit
# 2026-07-06: data alone left world/gameplay/quest records uncookable).
target_link_libraries(cooker PRIVATE meadows-data meadows-world
                      meadows-narrative)
meadows_set_warnings(cooker)
