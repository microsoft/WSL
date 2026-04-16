// +build cgo

package main

/*
#include <stdlib.h>
*/
import "C"

import (
    "bytes"
    "encoding/json"
    "text/template"
    "unsafe"
)

func toJSONString(v interface{}) (string, error) {
    b, err := json.Marshal(v)
    if err != nil {
        return "", err
    }

    return string(b), nil
}

// TryRenderGoTemplate renders a Go template with the provided JSON data.
// Returns 1 on success, 0 on failure. On success, *output contains the rendered result.
// On failure, *output contains the error message. The caller must free *output with FreeGoString.
//
//export TryRenderGoTemplate
func TryRenderGoTemplate(templateStr *C.char, jsonData *C.char, output **C.char) C.int {
    if templateStr == nil || jsonData == nil {
        *output = C.CString("null pointer provided")
        return 0
    }

    var data interface{}
    if err := json.Unmarshal([]byte(C.GoString(jsonData)), &data); err != nil {
        *output = C.CString("failed to parse JSON: " + err.Error())
        return 0
    }

    funcMap := template.FuncMap{
        "json": toJSONString,
    }

    tmpl, err := template.New("gotemplate").Funcs(funcMap).Parse(C.GoString(templateStr))
    if err != nil {
        *output = C.CString("failed to parse template: " + err.Error())
        return 0
    }

    var result bytes.Buffer
    if err = tmpl.Execute(&result, data); err != nil {
        *output = C.CString("failed to execute template: " + err.Error())
        return 0
    }

    *output = C.CString(result.String())
    return 1
}

// FreeGoString frees a string allocated by TryRenderGoTemplate.
//
//export FreeGoString
func FreeGoString(ptr *C.char) {
    C.free(unsafe.Pointer(ptr))
}

func main() {
}