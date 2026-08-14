#include "gisland/rlgl_texture_books.hpp"

#include <algorithm>
#include <cmath>
#include <compare>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <variant>

namespace gisland {
namespace {

struct FontKey {
  std::string resource;
  int base_size;

  auto operator<=>(const FontKey &) const = default;
};

struct PreparedImageKey {
  std::string resource_id;
  int width;
  int height;
  ImageFit fit;
  ImageShape shape;
  int radius;

  auto operator<=>(const PreparedImageKey &) const = default;
};

[[nodiscard]] int base_size(const TypographyRole &role) {
  return std::max(1, static_cast<int>(std::ceil(role.size)));
}

[[nodiscard]] PreparedImageKey image_key(const ImageDrawCommand &command) {
  return {command.resource_id,   command.bounds.width,
          command.bounds.height, command.style.fit,
          command.style.shape,   static_cast<int>(std::lround(command.style.radius))};
}

[[nodiscard]] bool valid_resource(const ImageResource &resource) {
  if (resource.width == 0 || resource.height == 0 || resource.pixels == nullptr ||
      resource.format != ImageFormat::rgba8) {
    return false;
  }
  const auto width = static_cast<std::size_t>(resource.width);
  const auto height = static_cast<std::size_t>(resource.height);
  return width <= std::numeric_limits<std::size_t>::max() / height &&
         width * height <= std::numeric_limits<std::size_t>::max() / 4U &&
         resource.pixels->size() == width * height * 4U;
}

[[nodiscard]] bool inside_mask(int x, int y, int width, int height, ImageShape shape,
                               double radius) {
  if (shape == ImageShape::rectangle) {
    return true;
  }
  const double pixel_x = static_cast<double>(x) + 0.5;
  const double pixel_y = static_cast<double>(y) + 0.5;
  if (shape == ImageShape::circle) {
    const double center_x = static_cast<double>(width) / 2.0;
    const double center_y = static_cast<double>(height) / 2.0;
    const double delta_x = pixel_x - center_x;
    const double delta_y = pixel_y - center_y;
    const double circle_radius = static_cast<double>(std::min(width, height)) / 2.0;
    return delta_x * delta_x + delta_y * delta_y <= circle_radius * circle_radius;
  }
  const double bounded =
      std::clamp(radius, 0.0, static_cast<double>(std::min(width, height)) / 2.0);
  const double closest_x = std::clamp(pixel_x, bounded, static_cast<double>(width) - bounded);
  const double closest_y = std::clamp(pixel_y, bounded, static_cast<double>(height) - bounded);
  const double delta_x = pixel_x - closest_x;
  const double delta_y = pixel_y - closest_y;
  return delta_x * delta_x + delta_y * delta_y <= bounded * bounded;
}

[[nodiscard]] std::vector<std::uint8_t> prepare_image_pixels(const ImageResource &resource,
                                                             const ImageDrawCommand &command) {
  const int output_width = command.bounds.width;
  const int output_height = command.bounds.height;
  std::vector<std::uint8_t> output(static_cast<std::size_t>(output_width) *
                                   static_cast<std::size_t>(output_height) * 4U);
  const double scale_x = static_cast<double>(output_width) / resource.width;
  const double scale_y = static_cast<double>(output_height) / resource.height;
  const double scale = command.style.fit == ImageFit::cover ? std::max(scale_x, scale_y)
                                                            : std::min(scale_x, scale_y);
  const double rendered_width = static_cast<double>(resource.width) * scale;
  const double rendered_height = static_cast<double>(resource.height) * scale;
  const double offset_x = (static_cast<double>(output_width) - rendered_width) / 2.0;
  const double offset_y = (static_cast<double>(output_height) - rendered_height) / 2.0;
  const auto channel = [&](int x, int y, std::size_t component) {
    const auto index =
        (static_cast<std::size_t>(y) * resource.width + static_cast<std::size_t>(x)) * 4U +
        component;
    return static_cast<double>(resource.pixels->at(index));
  };
  for (int y = 0; y < output_height; ++y) {
    for (int x = 0; x < output_width; ++x) {
      const auto output_index =
          (static_cast<std::size_t>(y) * output_width + static_cast<std::size_t>(x)) * 4U;
      const double center_x = static_cast<double>(x) + 0.5;
      const double center_y = static_cast<double>(y) + 0.5;
      if (center_x < offset_x || center_x >= offset_x + rendered_width || center_y < offset_y ||
          center_y >= offset_y + rendered_height ||
          !inside_mask(x, y, output_width, output_height, command.style.shape,
                       command.style.radius)) {
        continue;
      }
      const double source_x = std::clamp((center_x - offset_x) / scale - 0.5, 0.0,
                                         static_cast<double>(resource.width - 1U));
      const double source_y = std::clamp((center_y - offset_y) / scale - 0.5, 0.0,
                                         static_cast<double>(resource.height - 1U));
      const int x0 = static_cast<int>(std::floor(source_x));
      const int y0 = static_cast<int>(std::floor(source_y));
      const int x1 = std::min(x0 + 1, static_cast<int>(resource.width) - 1);
      const int y1 = std::min(y0 + 1, static_cast<int>(resource.height) - 1);
      const double fraction_x = source_x - x0;
      const double fraction_y = source_y - y0;
      for (std::size_t component = 0; component < 4U; ++component) {
        const double top =
            std::lerp(channel(x0, y0, component), channel(x1, y0, component), fraction_x);
        const double bottom =
            std::lerp(channel(x0, y1, component), channel(x1, y1, component), fraction_x);
        output[output_index + component] =
            static_cast<std::uint8_t>(std::lround(std::lerp(top, bottom, fraction_y)));
      }
    }
  }
  return output;
}

} // namespace

struct RlglFontTextureBook::Impl {
  struct Entry {
    const FontAtlasData *atlas;
    RlglTexture texture;
  };

  const RlglSession *session{};
  const RlglFontBook *fonts{};
  std::map<FontKey, Entry> entries;
  mutable RlglFontBinding binding{};
};

RlglFontTextureBook::RlglFontTextureBook(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
RlglFontTextureBook::RlglFontTextureBook(RlglFontTextureBook &&) noexcept = default;
RlglFontTextureBook &RlglFontTextureBook::operator=(RlglFontTextureBook &&) noexcept = default;
RlglFontTextureBook::~RlglFontTextureBook() = default;

std::expected<RlglFontTextureBook, RlglTextureBookError>
RlglFontTextureBook::load(const RlglSession &session, const RlglFontBook &fonts) {
  if (!session.current()) {
    return std::unexpected(RlglTextureBookError::gpu_error);
  }
  auto impl = std::make_unique<Impl>();
  impl->session = &session;
  impl->fonts = &fonts;
  return RlglFontTextureBook{std::move(impl)};
}

std::expected<void, RlglTextureBookError> RlglFontTextureBook::prepare(const LayoutPlan &plan) {
  for (const auto &content : plan.content) {
    const auto prepare_command =
        [this](const auto &command) -> std::expected<void, RlglTextureBookError> {
      using Command = std::decay_t<decltype(command)>;
      if constexpr (!std::is_same_v<Command, TextDrawCommand> &&
                    !std::is_same_v<Command, IconDrawCommand>) {
        return {};
      } else {
        const FontKey key{command.font_resource, base_size(command.typography)};
        if (impl_->entries.contains(key)) {
          return {};
        }
        const auto *atlas = impl_->fonts->atlas(command.font_resource, command.typography);
        if (atlas == nullptr) {
          return std::unexpected(RlglTextureBookError::missing_resource);
        }
        auto texture = RlglTexture::font_atlas(*impl_->session, *atlas, RlglTextureFilter::nearest);
        if (!texture) {
          return std::unexpected(RlglTextureBookError::gpu_error);
        }
        impl_->entries.emplace(key, Impl::Entry{atlas, std::move(*texture)});
        return {};
      }
    };
    auto prepared = std::visit(prepare_command, content);
    if (!prepared) {
      return prepared;
    }
  }
  return {};
}

const RlglFontBinding *RlglFontTextureBook::find(std::string_view resource,
                                                 const TypographyRole &role) const noexcept {
  const auto entry = impl_->entries.find(FontKey{std::string{resource}, base_size(role)});
  if (entry == impl_->entries.end()) {
    return nullptr;
  }
  impl_->binding = {entry->second.atlas, &entry->second.texture};
  return &impl_->binding;
}

std::size_t RlglFontTextureBook::loaded_texture_count() const noexcept {
  return impl_->entries.size();
}

struct RlglImageBook::Impl {
  struct Signature {
    std::uint32_t source_width;
    std::uint32_t source_height;
    std::shared_ptr<const std::vector<std::uint8_t>> pixels;
    int output_width;
    int output_height;
    ImageRole style;
    std::size_t texture_index;
  };

  const RlglSession *session{};
  std::map<std::string, ImageResource, std::less<>> resources;
  std::map<PreparedImageKey, std::size_t> bindings;
  std::vector<Signature> signatures;
  std::vector<RlglTexture> textures;
};

RlglImageBook::RlglImageBook(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
RlglImageBook::RlglImageBook(RlglImageBook &&) noexcept = default;
RlglImageBook &RlglImageBook::operator=(RlglImageBook &&) noexcept = default;
RlglImageBook::~RlglImageBook() = default;

std::expected<RlglImageBook, RlglTextureBookError>
RlglImageBook::load(const RlglSession &session, const std::vector<ImageResource> &resources) {
  if (!session.current()) {
    return std::unexpected(RlglTextureBookError::gpu_error);
  }
  auto impl = std::make_unique<Impl>();
  impl->session = &session;
  for (const auto &resource : resources) {
    if (!valid_resource(resource) || !impl->resources.emplace(resource.id, resource).second) {
      return std::unexpected(RlglTextureBookError::invalid_resource);
    }
  }
  return RlglImageBook{std::move(impl)};
}

std::expected<void, RlglTextureBookError> RlglImageBook::prepare(const LayoutPlan &plan) {
  for (const auto &content : plan.content) {
    const auto *command = std::get_if<ImageDrawCommand>(&content);
    if (command == nullptr) {
      continue;
    }
    const auto key = image_key(*command);
    if (impl_->bindings.contains(key)) {
      continue;
    }
    const auto resource = impl_->resources.find(command->resource_id);
    if (resource == impl_->resources.end()) {
      return std::unexpected(RlglTextureBookError::missing_resource);
    }
    if (command->bounds.width <= 0 || command->bounds.height <= 0) {
      return std::unexpected(RlglTextureBookError::invalid_geometry);
    }
    const auto shared = std::ranges::find_if(impl_->signatures, [&](const auto &signature) {
      return signature.source_width == resource->second.width &&
             signature.source_height == resource->second.height &&
             signature.output_width == command->bounds.width &&
             signature.output_height == command->bounds.height &&
             signature.style == command->style && *signature.pixels == *resource->second.pixels;
    });
    if (shared != impl_->signatures.end()) {
      impl_->bindings.emplace(key, shared->texture_index);
      continue;
    }
    auto pixels = prepare_image_pixels(resource->second, *command);
    auto texture = RlglTexture::rgba8(*impl_->session, command->bounds.width,
                                      command->bounds.height, pixels, RlglTextureFilter::linear);
    if (!texture) {
      return std::unexpected(RlglTextureBookError::gpu_error);
    }
    const std::size_t index = impl_->textures.size();
    impl_->textures.push_back(std::move(*texture));
    impl_->bindings.emplace(key, index);
    impl_->signatures.push_back({resource->second.width, resource->second.height,
                                 resource->second.pixels, command->bounds.width,
                                 command->bounds.height, command->style, index});
  }
  return {};
}

const RlglTexture *RlglImageBook::find(const ImageDrawCommand &command) const noexcept {
  const auto binding = impl_->bindings.find(image_key(command));
  return binding == impl_->bindings.end() || binding->second >= impl_->textures.size()
             ? nullptr
             : &impl_->textures[binding->second];
}

std::size_t RlglImageBook::loaded_texture_count() const noexcept { return impl_->textures.size(); }

struct RlglRichTextBook::Impl {
  struct Signature {
    RichText rich_text;
    int width;
    int height;
    std::size_t texture_index;
  };

  const RlglSession *session{};
  const PangoTextBook *text{};
  std::vector<ImageResource> resources;
  std::vector<Signature> signatures;
  std::vector<RlglTexture> textures;
};

RlglRichTextBook::RlglRichTextBook(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
RlglRichTextBook::RlglRichTextBook(RlglRichTextBook &&) noexcept = default;
RlglRichTextBook &RlglRichTextBook::operator=(RlglRichTextBook &&) noexcept = default;
RlglRichTextBook::~RlglRichTextBook() = default;

std::expected<RlglRichTextBook, RlglTextureBookError>
RlglRichTextBook::load(const RlglSession &session, const PangoTextBook &text,
                       const std::vector<ImageResource> &resources) {
  if (!session.current()) {
    return std::unexpected(RlglTextureBookError::gpu_error);
  }
  auto impl = std::make_unique<Impl>();
  impl->session = &session;
  impl->text = &text;
  impl->resources = resources;
  if (std::ranges::any_of(resources,
                          [](const auto &resource) { return !valid_resource(resource); })) {
    return std::unexpected(RlglTextureBookError::invalid_resource);
  }
  return RlglRichTextBook{std::move(impl)};
}

std::expected<void, RlglTextureBookError> RlglRichTextBook::prepare(const LayoutPlan &plan) {
  for (const auto &content : plan.content) {
    const auto *command = std::get_if<RichTextDrawCommand>(&content);
    if (command == nullptr) {
      continue;
    }
    if (command->bounds.width <= 0 || command->bounds.height <= 0) {
      return std::unexpected(RlglTextureBookError::invalid_geometry);
    }
    const auto existing = std::ranges::find_if(impl_->signatures, [command](const auto &candidate) {
      return candidate.width == command->bounds.width &&
             candidate.height == command->bounds.height &&
             candidate.rich_text == command->rich_text;
    });
    if (existing != impl_->signatures.end()) {
      continue;
    }
    auto surface =
        impl_->text->rasterize(command->rich_text, command->bounds.width, impl_->resources);
    if (!surface) {
      return std::unexpected(RlglTextureBookError::raster_failed);
    }
    if (surface->height != command->bounds.height) {
      return std::unexpected(RlglTextureBookError::invalid_geometry);
    }
    auto texture = RlglTexture::rgba8(*impl_->session, surface->width, surface->height,
                                      surface->pixels, RlglTextureFilter::linear);
    if (!texture) {
      return std::unexpected(RlglTextureBookError::gpu_error);
    }
    const std::size_t index = impl_->textures.size();
    impl_->textures.push_back(std::move(*texture));
    impl_->signatures.push_back(
        {command->rich_text, command->bounds.width, command->bounds.height, index});
  }
  return {};
}

const RlglTexture *RlglRichTextBook::find(const RichTextDrawCommand &command) const noexcept {
  const auto signature = std::ranges::find_if(impl_->signatures, [&command](const auto &candidate) {
    return candidate.width == command.bounds.width && candidate.height == command.bounds.height &&
           candidate.rich_text == command.rich_text;
  });
  return signature == impl_->signatures.end() || signature->texture_index >= impl_->textures.size()
             ? nullptr
             : &impl_->textures[signature->texture_index];
}

std::size_t RlglRichTextBook::loaded_texture_count() const noexcept {
  return impl_->textures.size();
}

} // namespace gisland
