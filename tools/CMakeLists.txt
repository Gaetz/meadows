# The cooker shares game/AllForms.cpp — the SINGLE registerAllFormTypes()
# aggregator — so the cooker and the game exe can never drift (a past drift
# left the cooker registering CoreForms only, silently dropping records).
# The file is headless (data/world/gameplay/quest only); world + narrative
# pull every form family into the link/include closure.
add_executable(cooker cooker/Main.cpp ${PROJECT_SOURCE_DIR}/game/AllForms.cpp)
target_link_libraries(cooker PRIVATE meadows-data meadows-world
                      meadows-narrative)
meadows_set_warnings(cooker)
