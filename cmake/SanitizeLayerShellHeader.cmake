file(READ "${INPUT}" content)
string(REPLACE "const char *namespace" "const char *namespace_name" content "${content}")
string(REPLACE ", namespace);" ", namespace_name);" content "${content}")
file(WRITE "${OUTPUT}" "${content}")
