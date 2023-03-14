#!/usr/bin/python3

"""
Generate custom/additional OLED screens for the Airgradient unit, and upload
the screen images to the unit automatically.

This (example) file pulls the metrics from the unit and generates the extra
screens directly for that. It only generates the text; one could also generate
graphs / trend data / etc by pulling more data out a local timeseries db, etc.
One could also tie this into other units such as thermostats or other systems
and display the data from that.
"""

import re
import time
from urllib import request, parse
from PIL import Image, ImageFont, ImageDraw

URL = 'http://PUT_YOUR_AIRGRAIDENT_IP_HERE:9926/uploadscreen'
METRICS_URL = 'http://PUT_YOUR_AIRGRADIENT_IP_HERE:9926/metrics'

# installed via the ubuntu fonts-freefont-ttf package
FONT = '/usr/share/fonts/truetype/freefont/FreeSansBold.ttf'

# Starting larger, and with different ratios for length/height
# this way the width is shrunk more / provides narrower text/numbers
# so more can fit on the screen.
START_CANVAS_SIZE = (128 * 3, 64 * 2)
ACTUAL_CANVAS_SIZE = (128, 64)


# if the images can be saved off to a file
ALLOW_IMAGES_TO_BE_SAVED = True

def AddCenterText(y, canvas_size_x, unicode_text, font, draw):
  text_width = font.getlength(unicode_text)
  x = int((canvas_size_x - text_width)/2)
  draw.text((x, y), unicode_text, 'white', font=font)


def BuildTwoLineImage(big_text, little_text):
  # one-color
  # build it larger, then scale it down
  canvas = Image.new('1', START_CANVAS_SIZE)
  draw = ImageDraw.Draw(canvas)

  # use a truetype font
  canvas_x = START_CANVAS_SIZE[0]
  min_size = 25*2
  # if the first size doesn't work, scale it down
  for font_size in (60*2, 56*2, 52*2, 40*2, 30*2, min_size):
    font = ImageFont.truetype(FONT, size=font_size)
    text_width = font.getlength(big_text)
    if font_size != min_size and text_width > canvas_x:
      # try a smaller size
      continue
    y_center = 2+int((START_CANVAS_SIZE[1]*.8 - font_size)/2)
    AddCenterText(y_center, canvas_x, big_text, font, draw)
    break

  # scale it down to the actual OLED size
  canvas = canvas.resize(ACTUAL_CANVAS_SIZE, resample=Image.BILINEAR)
  draw = ImageDraw.Draw(canvas)

  min_size = 8
  canvas_x = ACTUAL_CANVAS_SIZE[0]
  for font_size in (16, 14, 12, 10, min_size):
    font = ImageFont.truetype(FONT, size=font_size)
    text_width = font.getlength(little_text)
    if font_size != min_size and text_width > canvas_x:
      # try a smaller size
      continue
    AddCenterText(ACTUAL_CANVAS_SIZE[1]-font_size-1, canvas_x, little_text, font, draw)
    break

  return canvas


def BuildThreeLineImage(big_text, medium_text, little_text):
  # one-color, start larger
  canvas = Image.new('1', START_CANVAS_SIZE)
  draw = ImageDraw.Draw(canvas)

  # approx breakdown of rows
  # 55%
  # 30%
  # 15%

  canvas_x = START_CANVAS_SIZE[0]
  # 55% of 64*2 = 70.4
  min_size = 20*2
  for font_size in (40*2, 30*2, min_size):
    font = ImageFont.truetype(FONT, size=font_size)
    text_width = font.getlength(big_text)
    if font_size != min_size and text_width > canvas_x:
      # try a smaller size
      continue
    y_center = 2+int((START_CANVAS_SIZE[1]*.55 - font_size)/2)
    AddCenterText(y_center, canvas_x, big_text, font, draw)
    break

  # scale it down to the actual OLED size
  canvas = canvas.resize(ACTUAL_CANVAS_SIZE, resample=Image.BILINEAR)
  draw = ImageDraw.Draw(canvas)

  min_size = 8
  canvas_x = ACTUAL_CANVAS_SIZE[0]
  # 30% of 64= 19.2
  for font_size in (20, 16, 14, 12, 10, min_size):
    font = ImageFont.truetype(FONT, size=font_size)
    text_width = font.getlength(medium_text)
    if font_size != min_size and text_width > canvas_x:
      # try a smaller size
      continue
    y_center = 2+int(ACTUAL_CANVAS_SIZE[1]*0.55 + (ACTUAL_CANVAS_SIZE[1]*.3 - font_size)/2) - 2
    AddCenterText(y_center, canvas_x, medium_text, font, draw)
    break

  min_size = 8
  # 15% of 64 = 9.6
  canvas_x = ACTUAL_CANVAS_SIZE[0]
  for font_size in (12, 10, min_size):
    font = ImageFont.truetype(FONT, size=font_size)
    text_width = font.getlength(little_text)
    if font_size != min_size and text_width > canvas_x:
      # try a smaller size
      continue
    AddCenterText(ACTUAL_CANVAS_SIZE[1]-font_size-1, canvas_x, little_text, font, draw)
    break

  return canvas


def SaveImage(canvas, fn=None):
  pixels = canvas.load()
  col = []
  # do a manual calculation of pixels to XBM
  # PIL/Pillow has an XBM exporter but it exports c-code, not the actual binary
  # The image is tiny so this is fast
  for y in range(canvas.size[1]):
    byteval = 0
    bitoffset = 0
    for x in range(canvas.size[0]):
      if pixels[x, y] > 128:
        byteval += (1 << bitoffset)
      bitoffset += 1
      if bitoffset >= 8:
        col.append(byteval)
        byteval = 0
        bitoffset = 0
  ba = bytes(col)
  # it can also be saved off
  if ALLOW_IMAGES_TO_BE_SAVED and fn:
    canvas.save(fn, 'PNG')
  return ba



def Upload(post_val):
  data = parse.urlencode(post_val).encode()
  req =  request.Request(URL, data=data)
  with request.urlopen(req) as resp:
    result = resp.read().decode('utf-8')
    if 'ERROR' in result:
      print(resp.status, resp.reason)
      print(result)


def FetchMetrics():
  metrics = {}
  req =  request.Request(METRICS_URL)
  with request.urlopen(req) as resp:
    data = resp.read().decode('utf-8').split('\n')
    for line in data:
      if not line or line.startswith('#'):
        continue
      x = re.match('(.*?)({.*?})?[ \t]+(.*)', line)
      if not x:
        continue
      label = x.group(1)
      val = x.group(3)
      metrics[label] = val
  return metrics


# Example dict:
# {'aqi': '0', 'pm025': '0', 'nox': '1', 'tvoc': '102', 'rco2': '624', 'atmp': '68.72',
#  'rhum': '62', 'uptimesec': '642'}
# d0 ... is the order the images will appear
METRICS = {
 'aq': {
    'atmp': ['d0', lambda x: BuildTwoLineImage('%.1f°' % float(x), 'Temperature')],
    'rhum': ['d1', lambda x: BuildTwoLineImage('%d%%' % float(x), 'Humidity')],
    'aqi': ['d2', lambda x: BuildTwoLineImage('%d' % float(x), 'Air Quality Index')],
    'tvoc': ['d3', lambda x: BuildTwoLineImage('%d' % float(x), 'TVoC')],
    'rco2': ['d4', lambda x: BuildTwoLineImage('%d' % int(x), 'CO2')],
 }
}
# starting point for new values
START_D = 9
# {'temp_down': 65.9, 'temp_up': 67.4, 'temp_outside': 59.8}
#{'water': '21', 'watertoday': '74'}
#{'garagetime': '@6:15pm', 'garage': 'Shut'}
#{'watt': '933'}

def ProcessImage(dta, process_engine, field):
  post_val = {}
  canvas = process_engine[field][1](dta)
  post_val[process_engine[field][0]] = SaveImage(canvas, '%s.png' % process_engine[field][0])
  Upload(post_val)


def loop():
  metrics = FetchMetrics()
  for m in METRICS['aq']:
    if m in metrics:
      ProcessImage(metrics[m], METRICS['aq'], m)

  """
  More Examples:
  post_val = {}
  canvas = BuildTwoLineImage('1084W', 'Power, Current')
  post_val['d5'] = SaveImage(canvas, 'd5.png')
  Upload(post_val)

  post_val = {}
  canvas = BuildThreeLineImage('Closed', '@ 12:56pm', 'Garage')
  post_val['d6'] = SaveImage(canvas, 'd6.png')
  Upload(post_val)
  """

def main():
  while True:
    loop()
    # Update the screens this often. Could be more often, depends how live you want it.
    time.sleep(30)

main()
