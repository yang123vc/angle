//
// Copyright 2015 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// FramebufferGL.cpp: Implements the class methods for FramebufferGL.

#include "libANGLE/renderer/gl/FramebufferGL.h"

#include "common/BitSetIterator.h"
#include "common/debug.h"
#include "libANGLE/ContextState.h"
#include "libANGLE/State.h"
#include "libANGLE/FramebufferAttachment.h"
#include "libANGLE/angletypes.h"
#include "libANGLE/formatutils.h"
#include "libANGLE/renderer/ContextImpl.h"
#include "libANGLE/renderer/gl/FunctionsGL.h"
#include "libANGLE/renderer/gl/RenderbufferGL.h"
#include "libANGLE/renderer/gl/StateManagerGL.h"
#include "libANGLE/renderer/gl/TextureGL.h"
#include "libANGLE/renderer/gl/WorkaroundsGL.h"
#include "libANGLE/renderer/gl/formatutilsgl.h"
#include "platform/Platform.h"

using namespace gl;
using angle::CheckedNumeric;

namespace rx
{

namespace
{
gl::ErrorOrResult<bool> ShouldApplyLastRowPaddingWorkaround(const gl::Rectangle &area,
                                                            const gl::PixelPackState &pack,
                                                            GLenum format,
                                                            GLenum type,
                                                            const void *pixels)
{
    if (pack.pixelBuffer.get() == nullptr)
    {
        return false;
    }

    // We are using an pack buffer, compute what the driver thinks is going to be the last
    // byte written. If it is past the end of the buffer, we will need to use the workaround
    // otherwise the driver will generate INVALID_OPERATION.
    CheckedNumeric<size_t> checkedEndByte;
    CheckedNumeric<size_t> pixelBytes;
    size_t rowPitch;

    gl::Extents size(area.width, area.height, 1);
    const gl::InternalFormat &glFormat =
        gl::GetInternalFormatInfo(gl::GetSizedInternalFormat(format, type));
    ANGLE_TRY_RESULT(glFormat.computePackEndByte(type, size, pack), checkedEndByte);
    ANGLE_TRY_RESULT(glFormat.computeRowPitch(type, area.width, pack.alignment, pack.rowLength),
                     rowPitch);
    pixelBytes = glFormat.computePixelBytes(type);

    checkedEndByte += reinterpret_cast<intptr_t>(pixels);

    // At this point checkedEndByte is the actual last byte written.
    // The driver adds an extra row padding (if any), mimic it.
    ANGLE_TRY_CHECKED_MATH(pixelBytes);
    if (pixelBytes.ValueOrDie() * size.width < rowPitch)
    {
        checkedEndByte += rowPitch - pixelBytes * size.width;
    }

    ANGLE_TRY_CHECKED_MATH(checkedEndByte);

    return checkedEndByte.ValueOrDie() > static_cast<size_t>(pack.pixelBuffer->getSize());
}
}  // anonymous namespace

FramebufferGL::FramebufferGL(const FramebufferState &state,
                             const FunctionsGL *functions,
                             StateManagerGL *stateManager,
                             const WorkaroundsGL &workarounds,
                             bool isDefault)
    : FramebufferImpl(state),
      mFunctions(functions),
      mStateManager(stateManager),
      mWorkarounds(workarounds),
      mFramebufferID(0),
      mIsDefault(isDefault)
{
    if (!mIsDefault)
    {
        mFunctions->genFramebuffers(1, &mFramebufferID);
    }
}

FramebufferGL::FramebufferGL(GLuint id,
                             const FramebufferState &state,
                             const FunctionsGL *functions,
                             const WorkaroundsGL &workarounds,
                             StateManagerGL *stateManager)
    : FramebufferImpl(state),
      mFunctions(functions),
      mStateManager(stateManager),
      mWorkarounds(workarounds),
      mFramebufferID(id),
      mIsDefault(true)
{
}

FramebufferGL::~FramebufferGL()
{
    mStateManager->deleteFramebuffer(mFramebufferID);
    mFramebufferID = 0;
}

static void BindFramebufferAttachment(const FunctionsGL *functions,
                                      GLenum attachmentPoint,
                                      const FramebufferAttachment *attachment)
{
    if (attachment)
    {
        if (attachment->type() == GL_TEXTURE)
        {
            const Texture *texture     = attachment->getTexture();
            const TextureGL *textureGL = GetImplAs<TextureGL>(texture);

            if (texture->getTarget() == GL_TEXTURE_2D)
            {
                functions->framebufferTexture2D(GL_FRAMEBUFFER, attachmentPoint, GL_TEXTURE_2D,
                                                textureGL->getTextureID(), attachment->mipLevel());
            }
            else if (texture->getTarget() == GL_TEXTURE_CUBE_MAP)
            {
                functions->framebufferTexture2D(GL_FRAMEBUFFER, attachmentPoint, attachment->cubeMapFace(),
                                                textureGL->getTextureID(), attachment->mipLevel());
            }
            else if (texture->getTarget() == GL_TEXTURE_2D_ARRAY || texture->getTarget() == GL_TEXTURE_3D)
            {
                functions->framebufferTextureLayer(GL_FRAMEBUFFER, attachmentPoint, textureGL->getTextureID(),
                                                   attachment->mipLevel(), attachment->layer());
            }
            else
            {
                UNREACHABLE();
            }
        }
        else if (attachment->type() == GL_RENDERBUFFER)
        {
            const Renderbuffer *renderbuffer     = attachment->getRenderbuffer();
            const RenderbufferGL *renderbufferGL = GetImplAs<RenderbufferGL>(renderbuffer);

            functions->framebufferRenderbuffer(GL_FRAMEBUFFER, attachmentPoint, GL_RENDERBUFFER,
                                               renderbufferGL->getRenderbufferID());
        }
        else
        {
            UNREACHABLE();
        }
    }
    else
    {
        // Unbind this attachment
        functions->framebufferTexture2D(GL_FRAMEBUFFER, attachmentPoint, GL_TEXTURE_2D, 0, 0);
    }
}

Error FramebufferGL::discard(size_t count, const GLenum *attachments)
{
    UNIMPLEMENTED();
    return Error(GL_INVALID_OPERATION);
}

Error FramebufferGL::invalidate(size_t count, const GLenum *attachments)
{
    // Since this function is just a hint and not available until OpenGL 4.3, only call it if it is available.
    if (mFunctions->invalidateFramebuffer)
    {
        mStateManager->bindFramebuffer(GL_FRAMEBUFFER, mFramebufferID);
        mFunctions->invalidateFramebuffer(GL_FRAMEBUFFER, static_cast<GLsizei>(count), attachments);
    }

    return Error(GL_NO_ERROR);
}

Error FramebufferGL::invalidateSub(size_t count,
                                   const GLenum *attachments,
                                   const gl::Rectangle &area)
{
    // Since this function is just a hint and not available until OpenGL 4.3, only call it if it is available.
    if (mFunctions->invalidateSubFramebuffer)
    {
        mStateManager->bindFramebuffer(GL_FRAMEBUFFER, mFramebufferID);
        mFunctions->invalidateSubFramebuffer(GL_FRAMEBUFFER, static_cast<GLsizei>(count),
                                             attachments, area.x, area.y, area.width, area.height);
    }

    return Error(GL_NO_ERROR);
}

Error FramebufferGL::clear(ContextImpl *context, GLbitfield mask)
{
    syncClearState(mask);
    mStateManager->bindFramebuffer(GL_FRAMEBUFFER, mFramebufferID);
    mFunctions->clear(mask);

    return Error(GL_NO_ERROR);
}

Error FramebufferGL::clearBufferfv(ContextImpl *context,
                                   GLenum buffer,
                                   GLint drawbuffer,
                                   const GLfloat *values)
{
    syncClearBufferState(buffer, drawbuffer);
    mStateManager->bindFramebuffer(GL_FRAMEBUFFER, mFramebufferID);
    mFunctions->clearBufferfv(buffer, drawbuffer, values);

    return Error(GL_NO_ERROR);
}

Error FramebufferGL::clearBufferuiv(ContextImpl *context,
                                    GLenum buffer,
                                    GLint drawbuffer,
                                    const GLuint *values)
{
    syncClearBufferState(buffer, drawbuffer);
    mStateManager->bindFramebuffer(GL_FRAMEBUFFER, mFramebufferID);
    mFunctions->clearBufferuiv(buffer, drawbuffer, values);

    return Error(GL_NO_ERROR);
}

Error FramebufferGL::clearBufferiv(ContextImpl *context,
                                   GLenum buffer,
                                   GLint drawbuffer,
                                   const GLint *values)
{
    syncClearBufferState(buffer, drawbuffer);
    mStateManager->bindFramebuffer(GL_FRAMEBUFFER, mFramebufferID);
    mFunctions->clearBufferiv(buffer, drawbuffer, values);

    return Error(GL_NO_ERROR);
}

Error FramebufferGL::clearBufferfi(ContextImpl *context,
                                   GLenum buffer,
                                   GLint drawbuffer,
                                   GLfloat depth,
                                   GLint stencil)
{
    syncClearBufferState(buffer, drawbuffer);
    mStateManager->bindFramebuffer(GL_FRAMEBUFFER, mFramebufferID);
    mFunctions->clearBufferfi(buffer, drawbuffer, depth, stencil);

    return Error(GL_NO_ERROR);
}

GLenum FramebufferGL::getImplementationColorReadFormat() const
{
    const auto *readAttachment = mState.getReadAttachment();
    const Format &format       = readAttachment->getFormat();
    return format.info->getReadPixelsFormat();
}

GLenum FramebufferGL::getImplementationColorReadType() const
{
    const auto *readAttachment = mState.getReadAttachment();
    const Format &format       = readAttachment->getFormat();
    return format.info->getReadPixelsType();
}

Error FramebufferGL::readPixels(ContextImpl *context,
                                const gl::Rectangle &area,
                                GLenum format,
                                GLenum type,
                                GLvoid *pixels) const
{
    // TODO: don't sync the pixel pack state here once the dirty bits contain the pixel pack buffer
    // binding
    const PixelPackState &packState = context->getGLState().getPackState();
    mStateManager->setPixelPackState(packState);

    nativegl::ReadPixelsFormat readPixelsFormat =
        nativegl::GetReadPixelsFormat(mFunctions, mWorkarounds, format, type);
    GLenum readFormat = readPixelsFormat.format;
    GLenum readType   = readPixelsFormat.type;

    mStateManager->bindFramebuffer(GL_READ_FRAMEBUFFER, mFramebufferID);

    if (mWorkarounds.packOverlappingRowsSeparatelyPackBuffer && packState.pixelBuffer.get() &&
        packState.rowLength != 0 && packState.rowLength < area.width)
    {
        return readPixelsRowByRowWorkaround(area, readFormat, readType, packState, pixels);
    }

    if (mWorkarounds.packLastRowSeparatelyForPaddingInclusion)
    {
        bool apply;
        ANGLE_TRY_RESULT(
            ShouldApplyLastRowPaddingWorkaround(area, packState, readFormat, readType, pixels),
            apply);

        if (apply)
        {
            return readPixelsPaddingWorkaround(area, readFormat, readType, packState, pixels);
        }
    }

    mFunctions->readPixels(area.x, area.y, area.width, area.height, readFormat, readType, pixels);

    return gl::NoError();
}

Error FramebufferGL::blit(ContextImpl *context,
                          const gl::Rectangle &sourceArea,
                          const gl::Rectangle &destArea,
                          GLbitfield mask,
                          GLenum filter)
{
    const Framebuffer *sourceFramebuffer     = context->getGLState().getReadFramebuffer();
    const FramebufferGL *sourceFramebufferGL = GetImplAs<FramebufferGL>(sourceFramebuffer);

    mStateManager->bindFramebuffer(GL_READ_FRAMEBUFFER, sourceFramebufferGL->getFramebufferID());
    mStateManager->bindFramebuffer(GL_DRAW_FRAMEBUFFER, mFramebufferID);

    mFunctions->blitFramebuffer(sourceArea.x, sourceArea.y, sourceArea.x1(), sourceArea.y1(),
                                destArea.x, destArea.y, destArea.x1(), destArea.y1(), mask, filter);

    return Error(GL_NO_ERROR);
}

bool FramebufferGL::checkStatus() const
{
    mStateManager->bindFramebuffer(GL_FRAMEBUFFER, mFramebufferID);
    GLenum status = mFunctions->checkFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        ANGLEPlatformCurrent()->logWarning("GL framebuffer returned incomplete.");
    }
    return (status == GL_FRAMEBUFFER_COMPLETE);
}

void FramebufferGL::syncState(const Framebuffer::DirtyBits &dirtyBits)
{
    // Don't need to sync state for the default FBO.
    if (mIsDefault)
    {
        return;
    }

    mStateManager->bindFramebuffer(GL_FRAMEBUFFER, mFramebufferID);

    for (auto dirtyBit : angle::IterateBitSet(dirtyBits))
    {
        switch (dirtyBit)
        {
            case Framebuffer::DIRTY_BIT_DEPTH_ATTACHMENT:
                BindFramebufferAttachment(mFunctions, GL_DEPTH_ATTACHMENT,
                                          mState.getDepthAttachment());
                break;
            case Framebuffer::DIRTY_BIT_STENCIL_ATTACHMENT:
                BindFramebufferAttachment(mFunctions, GL_STENCIL_ATTACHMENT,
                                          mState.getStencilAttachment());
                break;
            case Framebuffer::DIRTY_BIT_DRAW_BUFFERS:
            {
                const auto &drawBuffers = mState.getDrawBufferStates();
                mFunctions->drawBuffers(static_cast<GLsizei>(drawBuffers.size()),
                                        drawBuffers.data());
                break;
            }
            case Framebuffer::DIRTY_BIT_READ_BUFFER:
                mFunctions->readBuffer(mState.getReadBufferState());
                break;
            default:
            {
                ASSERT(Framebuffer::DIRTY_BIT_COLOR_ATTACHMENT_0 == 0 &&
                       dirtyBit < Framebuffer::DIRTY_BIT_COLOR_ATTACHMENT_MAX);
                size_t index =
                    static_cast<size_t>(dirtyBit - Framebuffer::DIRTY_BIT_COLOR_ATTACHMENT_0);
                BindFramebufferAttachment(mFunctions,
                                          static_cast<GLenum>(GL_COLOR_ATTACHMENT0 + index),
                                          mState.getColorAttachment(index));
                break;
            }
        }
    }
}

GLuint FramebufferGL::getFramebufferID() const
{
    return mFramebufferID;
}

void FramebufferGL::syncDrawState() const
{
    if (mFunctions->standard == STANDARD_GL_DESKTOP)
    {
        // Enable SRGB blending for all framebuffers except the default framebuffer on Desktop
        // OpenGL.
        // When SRGB blending is enabled, only SRGB capable formats will use it but the default
        // framebuffer will always use it if it is enabled.
        // TODO(geofflang): Update this when the framebuffer binding dirty changes, when it exists.
        mStateManager->setFramebufferSRGBEnabled(!mIsDefault);
    }
}

void FramebufferGL::syncClearState(GLbitfield mask)
{
    if (mFunctions->standard == STANDARD_GL_DESKTOP)
    {
        if (mWorkarounds.doesSRGBClearsOnLinearFramebufferAttachments &&
            (mask & GL_COLOR_BUFFER_BIT) != 0 && !mIsDefault)
        {
            bool hasSRBAttachment = false;
            for (const auto &attachment : mState.getColorAttachments())
            {
                if (attachment.isAttached() && attachment.getColorEncoding() == GL_SRGB)
                {
                    hasSRBAttachment = true;
                    break;
                }
            }

            mStateManager->setFramebufferSRGBEnabled(hasSRBAttachment);
        }
        else
        {
            mStateManager->setFramebufferSRGBEnabled(!mIsDefault);
        }
    }
}

void FramebufferGL::syncClearBufferState(GLenum buffer, GLint drawBuffer)
{
    if (mFunctions->standard == STANDARD_GL_DESKTOP)
    {
        if (mWorkarounds.doesSRGBClearsOnLinearFramebufferAttachments && buffer == GL_COLOR &&
            !mIsDefault)
        {
            // If doing a clear on a color buffer, set SRGB blend enabled only if the color buffer
            // is an SRGB format.
            const auto &drawbufferState  = mState.getDrawBufferStates();
            const auto &colorAttachments = mState.getColorAttachments();

            const FramebufferAttachment *attachment = nullptr;
            if (drawbufferState[drawBuffer] >= GL_COLOR_ATTACHMENT0 &&
                drawbufferState[drawBuffer] < GL_COLOR_ATTACHMENT0 + colorAttachments.size())
            {
                size_t attachmentIdx =
                    static_cast<size_t>(drawbufferState[drawBuffer] - GL_COLOR_ATTACHMENT0);
                attachment = &colorAttachments[attachmentIdx];
            }

            if (attachment != nullptr)
            {
                mStateManager->setFramebufferSRGBEnabled(attachment->getColorEncoding() == GL_SRGB);
            }
        }
        else
        {
            mStateManager->setFramebufferSRGBEnabled(!mIsDefault);
        }
    }
}
gl::Error FramebufferGL::readPixelsRowByRowWorkaround(const gl::Rectangle &area,
                                                      GLenum format,
                                                      GLenum type,
                                                      const gl::PixelPackState &pack,
                                                      GLvoid *pixels) const
{
    intptr_t offset = reinterpret_cast<intptr_t>(pixels);

    const gl::InternalFormat &glFormat =
        gl::GetInternalFormatInfo(gl::GetSizedInternalFormat(format, type));
    GLuint rowBytes = 0;
    ANGLE_TRY_RESULT(glFormat.computeRowPitch(type, area.width, pack.alignment, pack.rowLength),
                     rowBytes);
    GLuint skipBytes = 0;
    ANGLE_TRY_RESULT(
        glFormat.computeSkipBytes(rowBytes, 0, 0, pack.skipRows, pack.skipPixels, false),
        skipBytes);

    gl::PixelPackState directPack;
    directPack.pixelBuffer = pack.pixelBuffer;
    directPack.alignment   = 1;
    mStateManager->setPixelPackState(directPack);
    directPack.pixelBuffer.set(nullptr);

    offset += skipBytes;
    for (GLint row = 0; row < area.height; ++row)
    {
        mFunctions->readPixels(area.x, row + area.y, area.width, 1, format, type,
                               reinterpret_cast<GLvoid *>(offset));
        offset += row * rowBytes;
    }

    return gl::NoError();
}

gl::Error FramebufferGL::readPixelsPaddingWorkaround(const gl::Rectangle &area,
                                                     GLenum format,
                                                     GLenum type,
                                                     const gl::PixelPackState &pack,
                                                     GLvoid *pixels) const
{
    const gl::InternalFormat &glFormat =
        gl::GetInternalFormatInfo(gl::GetSizedInternalFormat(format, type));
    GLuint rowBytes = 0;
    ANGLE_TRY_RESULT(glFormat.computeRowPitch(type, area.width, pack.alignment, pack.rowLength),
                     rowBytes);
    GLuint skipBytes = 0;
    ANGLE_TRY_RESULT(
        glFormat.computeSkipBytes(rowBytes, 0, 0, pack.skipRows, pack.skipPixels, false),
        skipBytes);

    // Get all by the last row
    if (area.height > 1)
    {
        mFunctions->readPixels(area.x, area.y, area.width, area.height - 1, format, type, pixels);
    }

    // Get the last row manually
    gl::PixelPackState directPack;
    directPack.pixelBuffer = pack.pixelBuffer;
    directPack.alignment   = 1;
    mStateManager->setPixelPackState(directPack);
    directPack.pixelBuffer.set(nullptr);

    intptr_t lastRowOffset =
        reinterpret_cast<intptr_t>(pixels) + skipBytes + (area.height - 1) * rowBytes;
    mFunctions->readPixels(area.x, area.y + area.height - 1, area.width, 1, format, type,
                           reinterpret_cast<GLvoid *>(lastRowOffset));

    return gl::NoError();
}
}  // namespace rx
