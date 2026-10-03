"""Generate the repository's code-drawn icon; Pillow is only a developer dependency."""
from pathlib import Path
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[1]
image = Image.new("RGBA", (256, 256))
d = ImageDraw.Draw(image)
d.rounded_rectangle((8, 8, 248, 248), 64, fill="#5968dc")
d.rounded_rectangle((88, 56, 172, 200), 40, outline="white", width=14)
d.line((128, 60, 128, 100), fill="white", width=14)
for line in ((48,80,64,88), (60,40,76,56), (184,40,172,56)):
    d.line(line, fill="white", width=12)
image.save(root / "assets" / "app.ico", sizes=[(16,16),(24,24),(32,32),(48,48),(64,64),(128,128),(256,256)])
arrow = Image.new("RGBA", (48, 48))
ImageDraw.Draw(arrow).line((10, 18, 24, 31, 38, 18), fill="#798397", width=5)
arrow.resize((12, 12), Image.Resampling.LANCZOS).save(root / "assets" / "chevron.png")
