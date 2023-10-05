use core::slice;
use std::{
    ffi::{c_int, CString, NulError},
    io::{self},
    mem::size_of,
    num::TryFromIntError,
    ptr::{self, copy_nonoverlapping},
};

use crate::{
    dp_error, DP_Image, DP_Output, DP_Quad, DP_UPixel8, DP_blend_color8_to,
    DP_file_output_new_from_path, DP_image_free, DP_image_height, DP_image_new,
    DP_image_new_subimage, DP_image_pixels, DP_image_transform_pixels, DP_image_width,
    DP_image_write_jpeg, DP_image_write_png, DP_output_free, DP_MSG_TRANSFORM_REGION_MODE_BILINEAR,
};

use super::DrawContext;

pub struct Image {
    image: *mut DP_Image,
}

#[derive(Debug)]
pub struct ImageError {
    pub message: String,
}

impl ImageError {
    pub fn from_dp_error() -> Self {
        Self {
            message: dp_error(),
        }
    }
}

impl From<&str> for ImageError {
    fn from(value: &str) -> Self {
        Self {
            message: value.to_owned(),
        }
    }
}

impl From<TryFromIntError> for ImageError {
    fn from(value: TryFromIntError) -> Self {
        Self {
            message: value.to_string(),
        }
    }
}

impl From<NulError> for ImageError {
    fn from(value: NulError) -> Self {
        Self {
            message: value.to_string(),
        }
    }
}

impl Image {
    pub fn new(width: usize, height: usize) -> Result<Self, ImageError> {
        if width > 0 && height > 0 {
            let w = c_int::try_from(width)?;
            let h = c_int::try_from(height)?;
            let image = unsafe { DP_image_new(w, h) };
            Ok(Self { image })
        } else {
            Err(ImageError::from("Empty image"))
        }
    }

    pub fn new_from_pixels(
        width: usize,
        height: usize,
        pixels: &[u32],
    ) -> Result<Self, ImageError> {
        let count = width * height;
        if pixels.len() >= count {
            let img = Self::new(width, height)?;
            unsafe {
                copy_nonoverlapping(
                    pixels.as_ptr(),
                    DP_image_pixels(img.image).cast::<u32>(),
                    count,
                );
            }
            Ok(img)
        } else {
            Err(ImageError::from("Not enough pixels"))
        }
    }

    pub fn new_from_pixels_scaled(
        width: usize,
        height: usize,
        pixels: &[u32],
        scale_width: usize,
        scale_height: usize,
        expand: bool,
        dc: &mut DrawContext,
    ) -> Result<Self, ImageError> {
        if width == 0 || height == 0 {
            return Err(ImageError::from("Empty source image"));
        }

        if scale_width == 0 || scale_height == 0 {
            return Err(ImageError::from("Empty target image"));
        }

        let count = width * height;
        if pixels.len() < count {
            return Err(ImageError::from("Not enough pixels"));
        }

        let xratio = scale_width as f64 / width as f64;
        let yratio = scale_height as f64 / height as f64;
        let (target_width, target_height) = if (xratio - yratio).abs() < 0.01 {
            (scale_width, scale_height)
        } else if xratio <= yratio {
            (scale_width, (height as f64 * xratio) as usize)
        } else {
            ((width as f64 * yratio) as usize, scale_height)
        };

        let right = c_int::try_from(target_width - 1)?;
        let bottom = c_int::try_from(target_height - 1)?;
        let dst_quad = DP_Quad {
            x1: 0,
            y1: 0,
            x2: right,
            y2: 0,
            x3: right,
            y3: bottom,
            x4: 0,
            y4: bottom,
        };

        let image = unsafe {
            DP_image_transform_pixels(
                c_int::try_from(width)?,
                c_int::try_from(height)?,
                pixels.as_ptr().cast(),
                dc.as_ptr(),
                &dst_quad,
                DP_MSG_TRANSFORM_REGION_MODE_BILINEAR as i32,
                ptr::null_mut(),
                ptr::null_mut(),
            )
        };
        if image.is_null() {
            return Err(ImageError::from_dp_error());
        }

        let img = Image { image };
        if expand && (target_width != scale_width || target_height != scale_height) {
            let subimg = unsafe {
                DP_image_new_subimage(
                    img.image,
                    -c_int::try_from((scale_width - target_width) / 2usize)?,
                    -c_int::try_from((scale_height - target_height) / 2usize)?,
                    c_int::try_from(scale_width)?,
                    c_int::try_from(scale_height)?,
                )
            };
            if subimg.is_null() {
                Err(ImageError::from_dp_error())
            } else {
                Ok(Image { image: subimg })
            }
        } else {
            Ok(img)
        }
    }

    pub fn width(&self) -> usize {
        unsafe { DP_image_width(self.image) as usize }
    }

    pub fn height(&self) -> usize {
        unsafe { DP_image_height(self.image) as usize }
    }

    pub fn dump(&self, writer: &mut dyn io::Write) -> io::Result<()> {
        let pixels = unsafe { DP_image_pixels(self.image) };
        let size = self.width() * self.height() * size_of::<u32>();
        writer.write_all(unsafe { slice::from_raw_parts(pixels.cast::<u8>(), size) })
    }

    pub fn write_png(&self, path: &str) -> Result<(), ImageError> {
        self.write(path, DP_image_write_png)
    }

    pub fn write_jpeg(&self, path: &str) -> Result<(), ImageError> {
        self.write(path, DP_image_write_jpeg)
    }

    fn write(
        &self,
        path: &str,
        func: unsafe extern "C" fn(*mut DP_Image, *mut DP_Output) -> bool,
    ) -> Result<(), ImageError> {
        let cpath = CString::new(path)?;
        let output = unsafe { DP_file_output_new_from_path(cpath.as_ptr()) };
        if output.is_null() {
            return Err(ImageError::from_dp_error());
        }
        let result = match unsafe { func(self.image, output) } {
            true => Ok(()),
            false => Err(ImageError::from_dp_error()),
        };
        unsafe { DP_output_free(output) };
        result
    }

    pub fn blend_with(
        &mut self,
        src: &Image,
        color: DP_UPixel8,
        opacity: u8,
    ) -> Result<(), ImageError> {
        let w = self.width();
        let h = self.height();
        if w != src.width() || h != src.height() {
            return Err(ImageError::from("Mismatched dimensions"));
        }

        unsafe {
            DP_blend_color8_to(
                DP_image_pixels(self.image),
                DP_image_pixels(src.image),
                color,
                (w * h) as i32,
                opacity,
            );
        }
        Ok(())
    }
}

impl Drop for Image {
    fn drop(&mut self) {
        unsafe { DP_image_free(self.image) }
    }
}