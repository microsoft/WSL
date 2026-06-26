# WSL open source documentation

Build instructions:

```
$ pip install -r doc/requirements.txt # or uv pip install -r doc/requirements.txt
$ python doc/build_versioned_docs.py --output public # or uv run doc/build_versioned_docs.py --output public
$ python -m http.server -d public 8000 # or uv run python -m http.server -d public 8000
```

You can then view the documentation at `http://127.0.0.1:8000/`.

The docs are two MkDocs projects: the main site (`mkdocs.yml`) and the
independently versioned API reference (`mkdocs-api.yml`). `build_versioned_docs.py`
builds the main site once, rebuilds the API reference for each release tag plus
the current branch, and merges them under `public/api-reference/`. To live-edit a
single project, run `mkdocs serve -f doc/mkdocs.yml` (or `-f doc/mkdocs-api.yml`).