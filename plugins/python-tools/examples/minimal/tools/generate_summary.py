def run(context):
    assets = context.inputs("assets/**/*")
    context.write_json("reports/project-summary.json", {
        "title": context.params["title"],
        "assetCount": len(assets),
        "assets": [
            path.relative_to(context.project_root).as_posix()
            for path in assets
        ],
    })
