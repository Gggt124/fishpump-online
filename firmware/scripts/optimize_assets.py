import os
import re
import gzip
import sys

def clean_js(source: str) -> str:
    """
    Pass-through: custom JS minification removed (2026-06-25).
    Reason: naive state machine did not handle ES6 template literals,
    causing silent truncation of '//' inside backtick strings.
    GZIP compression at build time provides sufficient size reduction.
    Future: use npm exec terser if minification is required.
    """
    return source

def clean_css(css: str) -> str:
    # CSS has no template literals — comment stripping is safe here.
    # Remove comments
    css = re.sub(r'/\*.*?\*/', '', css, flags=re.DOTALL)
    # Compress whitespaces
    css = re.sub(r'\s+', ' ', css)
    # Remove space around delimiters
    css = re.sub(r'\s*([\{\};:,])\s*', r'\1', css)
    return css.strip()

def clean_html(html: str) -> str:
    # HTML has no template literals — comment stripping is safe here.
    # Remove comments
    html = re.sub(r'<!--.*?-->', '', html, flags=re.DOTALL)
    # Collapse multiple spaces and newlines
    lines = []
    for line in html.splitlines():
        line_stripped = line.strip()
        if line_stripped:
            lines.append(line_stripped)
    return "\n".join(lines)

def compress_file(src_path: str, dest_raw_path: str, dest_gz_path: str, file_type: str) -> None:
    print(f"Optimizing {src_path}...")
    with open(src_path, "r", encoding="utf-8") as f:
        content = f.read()
    
    if file_type == "js":
        optimized = clean_js(content)
    elif file_type == "css":
        optimized = clean_css(content)
    elif file_type == "html":
        optimized = clean_html(content)
    else:
        optimized = content

    # Write optimized raw file
    with open(dest_raw_path, "w", encoding="utf-8") as f:
        f.write(optimized)
    
    # Write gzipped file (highest compression level 9)
    optimized_bytes = optimized.encode("utf-8")
    with gzip.open(dest_gz_path, "wb", compresslevel=9) as f:
        f.write(optimized_bytes)
        
    orig_size = os.path.getsize(src_path)
    opt_size = len(optimized_bytes)
    gz_size = os.path.getsize(dest_gz_path)
    print(f"  Original:   {orig_size:6} bytes")
    print(f"  Minified:   {opt_size:6} bytes ({opt_size/orig_size*100:.1f}%)" if orig_size > 0 else "  Minified:   0 bytes")
    print(f"  Gzipped:    {gz_size:6} bytes ({gz_size/orig_size*100:.1f}%)" if orig_size > 0 else "  Gzipped:    0 bytes")

def main():
    if "--help" in sys.argv or "-h" in sys.argv:
        print("Usage: python optimize_assets.py")
        print("Minifies and gzips static web assets (HTML, CSS, JS) from main/static to main/static_compressed.")
        return 0

    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.abspath(os.path.join(script_dir, ".."))
    
    src_dir = os.path.join(project_root, "main", "static")
    dest_dir = os.path.join(project_root, "main", "static_compressed")
    
    os.makedirs(dest_dir, exist_ok=True)
    
    files_to_process = [
        ("index.html", "html"),
        ("style.css", "css"),
        ("app.js", "js")
    ]
    
    for filename, file_type in files_to_process:
        src = os.path.join(src_dir, filename)
        dest_raw = os.path.join(dest_dir, filename)
        dest_gz = os.path.join(dest_dir, f"{filename}.gz")
        if not os.path.exists(src):
            print(f"Warning: Source file not found: {src}")
            continue
        compress_file(src, dest_raw, dest_gz, file_type)

    return 0

if __name__ == "__main__":
    sys.exit(main())
