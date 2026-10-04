# GeneralsX @feature Android port 04/10/2026 Rooms relay image: a static binary on scratch.
FROM golang:1.24-alpine AS build
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY *.go ./
ARG VERSION=dev
RUN CGO_ENABLED=0 go build -trimpath -ldflags "-s -w -X main.version=${VERSION}" -o /relay .

FROM scratch
COPY --from=build /relay /relay
USER 65534:65534
EXPOSE 8080
ENTRYPOINT ["/relay"]
