# meadows-narrative — quests (4e) and dialogue (4f): NarrativePro's node model
# decomposed into Form records, driven by the gameplay event bus + condition
# evaluator. Depends on gameplay (events/conditions/tags) + data; render-free.
add_library(meadows-narrative STATIC
    Quest.cpp
    Dialogue.cpp
)

target_link_libraries(meadows-narrative PUBLIC meadows-gameplay meadows-data)

meadows_set_warnings(meadows-narrative)
