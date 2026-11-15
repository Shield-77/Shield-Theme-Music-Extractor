Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'

function New-RoundPath([float]$X,[float]$Y,[float]$Width,[float]$Height,[float]$Radius) {
    $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $d = 2 * $Radius
    $path.AddArc($X,$Y,$d,$d,180,90)
    $path.AddArc($X+$Width-$d,$Y,$d,$d,270,90)
    $path.AddArc($X+$Width-$d,$Y+$Height-$d,$d,$d,0,90)
    $path.AddArc($X,$Y+$Height-$d,$d,$d,90,90)
    $path.CloseFigure()
    return $path
}

$root = Split-Path -Parent $PSScriptRoot
$navy = [System.Drawing.Color]::FromArgb(255,5,13,25)
$cyan = [System.Drawing.Color]::FromArgb(255,47,211,242)
$white = [System.Drawing.Color]::FromArgb(255,246,249,255)
$muted = [System.Drawing.Color]::FromArgb(255,165,184,205)
$fontFamily = [System.Drawing.FontFamily]::new('Segoe UI Semibold')

# Square Home Menu icon: music note inside a cyan theme folder.
$iconLarge = [System.Drawing.Bitmap]::new(192,192)
$g = [System.Drawing.Graphics]::FromImage($iconLarge)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.Clear($navy)
$gradient = [System.Drawing.Drawing2D.LinearGradientBrush]::new([System.Drawing.Rectangle]::new(0,0,192,192),[System.Drawing.Color]::FromArgb(255,15,39,61),$navy,45)
$g.FillPath($gradient,(New-RoundPath 4 4 184 184 34))
$gradient.Dispose()
$ring = [System.Drawing.Pen]::new($cyan,5)
$g.DrawEllipse($ring,28,28,136,136)
$ring.Dispose()
$folder = [System.Drawing.SolidBrush]::new($cyan)
$light = [System.Drawing.SolidBrush]::new($white)
$notePen = [System.Drawing.Pen]::new($white,10)
$shape = New-RoundPath 57 91 78 50 10
$g.FillPath($folder,$shape)
$g.FillRectangle($folder,63,80,34,20)
$g.DrawLine($notePen,106,62,106,98)
$g.DrawLine($notePen,106,62,129,56)
$g.FillEllipse($light,91,91,26,19)
$icon = [System.Drawing.Bitmap]::new(48,48)
$gi = [System.Drawing.Graphics]::FromImage($icon)
$gi.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$gi.DrawImage($iconLarge,0,0,48,48)
$icon.Save((Join-Path $root 'icon.png'),[System.Drawing.Imaging.ImageFormat]::Png)
$gi.Dispose(); $icon.Dispose(); $shape.Dispose(); $notePen.Dispose(); $folder.Dispose(); $light.Dispose(); $g.Dispose(); $iconLarge.Dispose()

# 3DS banner (256x128), with the same dark navy and cyan palette.
$bannerLarge = [System.Drawing.Bitmap]::new(512,256)
$b = [System.Drawing.Graphics]::FromImage($bannerLarge)
$b.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$b.Clear($navy)
$bg = [System.Drawing.Drawing2D.LinearGradientBrush]::new([System.Drawing.Rectangle]::new(0,0,512,256),[System.Drawing.Color]::FromArgb(255,12,34,56),[System.Drawing.Color]::FromArgb(255,3,10,20),0)
$b.FillRectangle($bg,0,0,512,256); $bg.Dispose()
$border = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(255,35,65,91),3)
$b.DrawRectangle($border,7,7,498,242); $border.Dispose()
$ring2 = [System.Drawing.Pen]::new($cyan,6)
$b.DrawEllipse($ring2,33,51,152,152); $ring2.Dispose()
$folder2 = [System.Drawing.SolidBrush]::new($cyan)
$white2 = [System.Drawing.SolidBrush]::new($white)
$notePen2 = [System.Drawing.Pen]::new($white,13)
$folderPath = New-RoundPath 65 115 88 58 11
$b.FillPath($folder2,$folderPath); $b.FillRectangle($folder2,73,102,38,24)
$b.DrawLine($notePen2,123,72,123,121); $b.DrawLine($notePen2,123,72,151,65)
$b.FillEllipse($white2,105,115,29,22)
$titleFont = [System.Drawing.Font]::new($fontFamily,40,[System.Drawing.FontStyle]::Bold,[System.Drawing.GraphicsUnit]::Pixel)
$subFont = [System.Drawing.Font]::new($fontFamily,18,[System.Drawing.FontStyle]::Regular,[System.Drawing.GraphicsUnit]::Pixel)
$cyanBrush = [System.Drawing.SolidBrush]::new($cyan)
$mutedBrush = [System.Drawing.SolidBrush]::new($muted)
$b.DrawString('SHIELD',$titleFont,$white2,210,57)
$b.DrawString('THEME MUSIC',$subFont,$cyanBrush,214,111)
$tagline = 'ÉCOUTE  •  COPIE  •  PROFITE'
$b.DrawString($tagline,$subFont,$mutedBrush,214,151)
for($i=0;$i -lt 12;$i++) {
    $height = 18 + (($i*23)%42)
    $b.FillRectangle($cyanBrush,214+$i*22,224-$height,12,$height)
}
$banner = [System.Drawing.Bitmap]::new(256,128)
$gb = [System.Drawing.Graphics]::FromImage($banner)
$gb.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$gb.DrawImage($bannerLarge,0,0,256,128)
$banner.Save((Join-Path $root 'banner.png'),[System.Drawing.Imaging.ImageFormat]::Png)
$gb.Dispose(); $banner.Dispose(); $bannerLarge.Dispose(); $folderPath.Dispose(); $folder2.Dispose(); $white2.Dispose(); $notePen2.Dispose(); $titleFont.Dispose(); $subFont.Dispose(); $cyanBrush.Dispose(); $mutedBrush.Dispose(); $b.Dispose()
