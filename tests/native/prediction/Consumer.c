#include <lv_advanced_keyboard_prediction.h>
#include <stdio.h>

int main(void)
{
    const void* trie = NULL;
    const void* index = NULL;
    size_t trie_size = 0;
    size_t index_size = 0;
    if (!lv_advanced_keyboard_get_builtin_trie(&trie, &trie_size, &index, &index_size) ||
        !trie || !index || trie_size < 16 || index_size < 16 ||
        lv_advanced_keyboard_predictor_workspace_bytes() == 0) return 1;
    lv_advanced_keyboard_predictor_t* predictor =
        lv_advanced_keyboard_predictor_create_borrowed(trie, trie_size, index, index_size);
    if (!predictor) return 2;
    lv_advanced_keyboard_suggestion_t suggestions[3] = {0};
    const size_t count = lv_advanced_keyboard_predictor_suggest_folded(
        predictor, "th", 2, NULL, 0, NULL, 0, suggestions, 3);
    lv_advanced_keyboard_predictor_destroy(predictor);
    if (count == 0 || count > 3 || suggestions[0].length == 0) return 3;
    puts("Standalone public C prediction API compiled, linked and returned suggestions");
    return 0;
}
