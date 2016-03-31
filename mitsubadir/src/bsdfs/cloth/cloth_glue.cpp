// vim makeprg=scons
    /*
       This file is part of Mitsuba, a physically based rendering system.

       Copyright (c) 2007-2014 by Wenzel Jakob and others.

       Mitsuba is free software; you can redistribute it and/or modify
       it under the terms of the GNU General Public License Version 3
       as published by the Free Software Foundation.

       Mitsuba is distributed in the hope that it will be useful,
       but WITHOUT ANY WARRANTY; without even the implied warranty of
       MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
       GNU General Public License for more details.

       You should have received a copy of the GNU General Public License
       along with this program. If not, see <http://www.gnu.org/licenses/>.
     */

    //#define DO_DEBUG
    //#define USE_WIFFILE

#include <mitsuba/render/scene.h>
#include <mitsuba/render/bsdf.h>
#include <mitsuba/render/texture.h>
#include <mitsuba/hw/basicshader.h>
#include <mitsuba/core/warp.h>
#include <mitsuba/core/fresolver.h>
//#include <mitsuba/render/noise.h>
//#include <mitsuba/hw/gpuprogram.h>
//#include <mitsuba/core/random.h>
#include <mitsuba/core/qmc.h>

#include "src/woven_cloth.h"

    /*static uint64_t rdtsc(){
        unsigned int lo,hi;
        __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
        return ((uint64_t)hi << 32) | lo;
    }*/

    //TODO(Vidar): Enable floating point exceptions

    MTS_NAMESPACE_BEGIN

    //TODO(Vidar): Write documentation

    class Cloth : public BSDF {
        public:
            Cloth(const Properties &props) : BSDF(props) {
                //Get and set parameters.

                // Reflectance is used to modify the color of the cloth
                /* For better compatibility with other models, support both
                   'reflectance' and 'diffuseReflectance' as parameter names */
                m_reflectance = new ConstantSpectrumTexture(props.getSpectrum(
                            props.hasProperty("reflectance") ? "reflectance"
                            : "diffuseReflectance", Spectrum(.5f)));

                m_weave_parameters.usacle = props.getFloat("utiling", 1.0f);
                m_weave_parameters.vscale = props.getFloat("vtiling", 1.0f);
                m_weave_parameters.umax = props.getFloat("umax", 0.7f);
                m_weave_parameters.psi = props.getFloat("psi", M_PI_2);
                m_weave_parameters.alpha = props.getFloat("alpha", 0.05f);  //uniform scattering
                m_weave_parameters.beta = props.getFloat("beta", 2.0f);     //forward scattering
                m_weave_parameters.delta_x = props.getFloat("deltaX", 0.5f);
                m_weave_parameters.specular_strength = 
                    props.getFloat("specular_strength", 0.5f);
                m_weave_parameters.specular_normalization = 1.f;

                //TODO(Peter): fix random
                m_intensity_fineness = props.getFloat("intensity_fineness", 0.0f);

                // LOAD WIF Pattern
#ifdef USE_WIFFILE
                std::string wiffilename =
                    Thread::getThread()->getFileResolver()->
                    resolve(props.getString("wiffile")).string();
                const char *filename = wiffilename.c_str();
                WeaveData *data = wif_read_wchar(filename);

                m_weave_parameters.pattern_entry = wif_get_pattern(data,
                        &m_weave_parameters.pattern_width,
                        &m_weave_parameters.pattern_height);
                wif_free_weavedata(data);
#else
                // Static pattern
                // current: polyester pattern
                uint8_t warp_above[] = {
                    0, 1, 1,
                    1, 0, 1,
                    1, 1, 0,
                };
                float warp_color[] = { 0.7f, 0.7f, 0.7f};
                float weft_color[] = { 0.7f, 0.7f, 0.7f};
                m_weave_parameters.pattern_width = 3;
                m_weave_parameters.pattern_height = 3;
                m_weave_parameters.pattern_entry = 
                    wif_build_pattern_from_data(warp_above,
                            warp_color, weft_color, m_pattern_width,
                            m_pattern_height);
#endif

                {
                    //Calculate normalization factor for the specular reflection
                    //Irawan:
                    /* Estimate the average reflectance under diffuse
                       illumination and use it to normalize the specular
                       component */
                    ref<Random> random = new Random();
                    size_t nSamples = 10000;
                    Intersection its;
                    BSDFSamplingRecord bRec(its, NULL, ERadiance);
                    float result = 0.0f;
                    for (size_t i=0; i<nSamples; ++i) {
                        bRec.wi = warp::squareToCosineHemisphere(Point2(random->nextFloat(), random->nextFloat()));
                        bRec.wo = warp::squareToCosineHemisphere(Point2(random->nextFloat(), random->nextFloat()));
                        its.uv = Point2(random->nextFloat(), random->nextFloat());
                        its.dpdv = Vector(1.f, 0.f, 0.f);
                        its.dpdu = Vector(0.f, 1.f, 0.f);

                        PatternData pattern_data = getPatternData(its);
                        result += specularReflectionPattern(
                                bRec.wi, bRec.wo, pattern_data,its);
                    }

                    if (result == 0.0001f){
                        m_weave_parameters.specular_normalization = 0.f;
                    }else{
                        m_weave_parameters.specular_normalization =
                            nSamples / (result * M_PI);
                    }
                }

            }

            Cloth(Stream *stream, InstanceManager *manager)
                : BSDF(stream, manager) {
                    //TODO(Vidar):Read parameters from stream
                    m_reflectance = static_cast<Texture *>(manager->getInstance(stream));

                    configure();
                }
            ~Cloth() {
                wif_free_pattern(m_weave_parameters.pattern_data);
            }

        void configure() {
            /* Verify the input parameter and fix them if necessary */
            m_reflectance = ensureEnergyConservation(m_reflectance, "reflectance", 1.0f);

            m_components.clear();
            m_components.push_back(EDiffuseReflection | EFrontSide
                    | ESpatiallyVarying);
            m_usesRayDifferentials = true;

            BSDF::configure();
        }

        Spectrum getDiffuseReflectance(const Intersection &its) const {
            PatternData pattern_data = getPatternData(its);
            return pattern_data.color * m_reflectance->eval(its);
        }

        //can go
        struct PatternData
        {
            Spectrum color;
            Frame frame; //The perturbed frame 
            float u, v; //Segment uv coordinates (in angles)
            float length, width; //Segment length and width
            float x, y; //position within segment. 
            float total_x, total_y; //index for elements. 
            bool warp_above; 
        };

        //can go
        void calculateLengthOfSegment(bool warp_above, uint32_t pattern_x,
                uint32_t pattern_y, uint32_t *steps_left,
                uint32_t *steps_right) const
        {

            uint32_t current_x = pattern_x;
            uint32_t current_y = pattern_y;
            uint32_t *incremented_coord = warp_above ? &current_y : &current_x;
            uint32_t max_size = warp_above ? m_pattern_height: m_pattern_width;
            uint32_t initial_coord = warp_above ? pattern_y: pattern_x;
            *steps_right = 0;
            *steps_left  = 0;
            do{
                (*incremented_coord)++;
                if(*incremented_coord == max_size){
                    *incremented_coord = 0;
                }
                if(m_pattern_entry[current_x +
                        current_y*m_pattern_width].warp_above != warp_above){
                    break;
                }
                (*steps_right)++;
            } while(*incremented_coord != initial_coord);

            *incremented_coord = initial_coord;
            do{
                if(*incremented_coord == 0){
                    *incremented_coord = max_size;
                }
                (*incremented_coord)--;
                if(m_pattern_entry[current_x +
                        current_y*m_pattern_width].warp_above != warp_above){
                    break;
                }
                (*steps_left)++;
            } while(*incremented_coord != initial_coord);
        }

        //can go
        PatternData getPatternData(const Intersection &its) const {

            //Set repeating uv coordinates.
            float u_repeat = fmod(its.uv.x*m_uscale,1.f);
            float v_repeat = fmod(its.uv.y*m_vscale,1.f);

            //pattern index
            //TODO(Peter): these are new. perhaps they can be used later 
            // to avoid duplicate calculations.
            //TODO(Peter): come up with a better name for these...
            uint32_t total_x = its.uv.x*m_uscale*m_pattern_width;
            uint32_t total_y = its.uv.y*m_vscale*m_pattern_height;

            //TODO(Vidar): Check why this crashes sometimes
            if (u_repeat < 0.f) {
                u_repeat = u_repeat - floor(u_repeat);
            }
            if (v_repeat < 0.f) {
                v_repeat = v_repeat - floor(v_repeat);
            }

            uint32_t pattern_x = (uint32_t)(u_repeat*(float)(m_pattern_width));
            uint32_t pattern_y = (uint32_t)(v_repeat*(float)(m_pattern_height));

            AssertEx(pattern_x < m_pattern_width, "pattern_x larger than pwidth");
            AssertEx(pattern_y < m_pattern_height, "pattern_y larger than pheight");

            PaletteEntry current_point = m_pattern_entry[pattern_x +
                pattern_y*m_pattern_width];        

            //Calculate the size of the segment
            uint32_t steps_left_warp = 0, steps_right_warp = 0;
            uint32_t steps_left_weft = 0, steps_right_weft = 0;
            if (current_point.warp_above) {
                calculateLengthOfSegment(current_point.warp_above, pattern_x,
                        pattern_y, &steps_left_warp, &steps_right_warp);
            }else{
                calculateLengthOfSegment(current_point.warp_above, pattern_x,
                        pattern_y, &steps_left_weft, &steps_right_weft);
            }

            //Yarn-segment-local coordinates.
            float l = (steps_left_warp + steps_right_warp + 1.f);
            float y = ((v_repeat*(float)(m_pattern_height) - (float)pattern_y)
                    + steps_left_warp)/l;

            float w = (steps_left_weft + steps_right_weft + 1.f);
            float x = ((u_repeat*(float)(m_pattern_width) - (float)pattern_x)
                    + steps_left_weft)/w;

            //Rescale x and y to [-1,1]
            x = x*2.f - 1.f;
            y = y*2.f - 1.f;

            //Switch X and Y for warp, so that we always have the yarn
            // cylinder going along the y axis
            if(!current_point.warp_above){
                float tmp1 = x;
                float tmp2 = w;
                x = -y;
                y = tmp1;
                w = l;
                l = tmp2;
            }

            //Calculate the yarn-segment-local u v coordinates along the curved cylinder
            //NOTE: This is different from how Irawan does it
            /*segment_u = asinf(x*sinf(m_umax));
              segment_v = asinf(y);*/
            //TODO(Vidar): Use a parameter for choosing model?
            float segment_u = y*m_umax;
            float segment_v = x*M_PI_2;

            //Calculate the normal in thread-local coordinates
            Vector normal(sinf(segment_v), sinf(segment_u)*cosf(segment_v),
                cosf(segment_u)*cosf(segment_v));
            
            //Calculate the tangent vector in thread-local coordinates
            Vector tangent(0.f, cosf(segment_u)*cosf(segment_v),
                cosf(segment_u)*cosf(segment_v));

            //Transform the normal back to shading space
            if(!current_point.warp_above){
                float tmp = normal.x;
                normal.x = normal.y;
                normal.y = -tmp;
            }

            //Get the world space coordinate vectors going along the texture u&v
            //TODO(Peter) Is it world space though!??!
            //NOTE(Vidar) You're right, it's probably shading space... :S
            Float dDispDu = normal.x;
            Float dDispDv = normal.y;
            Vector dpdv = its.dpdv + its.shFrame.n * (
                    -dDispDv - dot(its.shFrame.n, its.dpdv));
            Vector dpdu = its.dpdu + its.shFrame.n * (
                    -dDispDu - dot(its.shFrame.n, its.dpdu));
            // dpdv & dpdu are in world space

            //set frame
            Frame result;
            result.n = normalize(cross(dpdu, dpdv));

            result.s = normalize(dpdu - result.n
                    * dot(result.n, dpdu));
            result.t = cross(result.n, result.s);

            //Flip the normal if it points in the wrong direction
            if (dot(result.n, its.geoFrame.n) < 0)
                result.n *= -1;

            PatternData ret_data = {};
            ret_data.frame = result;
            ret_data.color.fromSRGB(current_point.color[0], current_point.color[1],
                    current_point.color[2]);

            ret_data.u = segment_u;
            ret_data.v = segment_v;
            ret_data.length = l;
            ret_data.width = w;
            ret_data.x = x; 
            ret_data.y = y; 
            ret_data.warp_above = current_point.warp_above; 
            ret_data.total_x = total_x;
            ret_data.total_y = total_y; 

            //return the results
            return ret_data;
        }


        //TODO(Peter): Implement in abstract 
        float intensityVariation(PatternData pattern_data) const {
            // have index to make a grid of finess*fineness squares 
            // of which to have the same brightness variations.
            
            //TODO(Peter): Clean this up a bit...
            //segment start x,y
            float startx = pattern_data.total_x - pattern_data.x*pattern_data.width;
            float starty = pattern_data.total_y - pattern_data.y*pattern_data.length;
            float centerx = startx + pattern_data.width/2.0;
            float centery = starty + pattern_data.length/2.0;
            
            uint32_t r1 = (uint32_t) ((centerx + pattern_data.total_x) 
                    * m_intensity_fineness);
            uint32_t r2 = (uint32_t) ((centery + pattern_data.total_y) 
                    * m_intensity_fineness);
 
            //srand(r1+r2); //bad way to do it?
            //float xi = rand();
		    //return fmin(-math::fastlog(xi), (float) 10.0f);
			
            float xi = sampleTEAFloat(r1, r2, 8);
			return std::min(-math::fastlog(xi), (Float) 10.0f);
        }

        //can go
        float specularReflectionPattern(Vector wi, Vector wo, PatternData data, Intersection its) const {
            float reflection = 0.f;
            
            // Depending on the given psi parameter the yarn is considered
            // staple or filament. They are treated differently in order
            // to work better numerically. 
            //TODO(Vidar): We should probably not check equality to 0.0f
            if (m_psi == 0.0) {
                //Filament yarn
                reflection = evalFilamentSpecular(wi, wo, data, its); 
            } else {
                //Staple yarn
                reflection = evalStapleSpecular(wi, wo, data, its); 
            }
            return reflection;
        }

        // can go
        float evalFilamentSpecular(Vector wi, Vector wo, PatternData data, Intersection its) const {
            // Half-vector, for some reason it seems to already be in the
            // correct coordinate frame... 
            if(!data.warp_above){
                //float tmp1 = H.x;
                float tmp2 = wi.x;
                float tmp3 = wo.x;
                wi.x = -wi.y; wi.y = tmp2;
                wo.x = -wo.y; wo.y = tmp3;
            }
            Vector H = normalize(wi + wo);

            float v = data.v;
            float y = data.y;

            //TODO(Peter): explain from where these expressions come.
            //compute v from x using (11). Already done. We have it from data.
            //compute u(wi,v,wr) -- u as function of v. using (4)...
            float specular_u = atan2f(-H.z, H.y) + M_PI_2; //plus or minus in last t.
            //TODO(Peter): check that it indeed is just v that should be used 
            //to calculate Gu (6) in Irawans paper.
            //calculate yarn tangent.

            float reflection = 0.f;
            if (fabsf(specular_u) < m_umax) {
                // Make normal for highlights, uses v and specular_u
                Vector highlight_normal = normalize(Vector(sinf(v),
                            sinf(specular_u)*cosf(v),
                            cosf(specular_u)*cosf(v)));

                // Make tangent for highlights, uses v and specular_u
                Vector highlight_tangent = normalize(Vector(0.f, 
                            cosf(specular_u), -sinf(specular_u)));

                //get specular_y, using irawans transformation.
                float specular_y = specular_u/m_umax;
                // our transformation TODO(Peter): Verify!
                //float specular_y = sinf(specular_u)/sinf(m_umax);

                //Clamp specular_y TODO(Peter): change name of m_delta_x to m_delta_h
                specular_y = specular_y < 1.f - m_delta_x ? specular_y :
                    1.f - m_delta_x;
                specular_y = specular_y > -1.f + m_delta_x ? specular_y :
                    -1.f + m_delta_x;

                if (fabsf(specular_y - y) < m_delta_x) { //this takes the role of xi in the irawan paper.
                    // --- Set Gu, using (6)
                    float a = 1.f; //radius of yarn
                    float R = 1.f/(sin(m_umax)); //radius of curvature
                    float Gu = a*(R + a*cosf(v)) / 
                        ((wi + wo).length() * fabsf(cross(highlight_tangent,H).x));

                    // --- Set fc
                    float cos_x = -dot(wi, wo);
                    float fc = m_alpha + vonMises(cos_x, m_beta);

                    // --- Set A
                    float widotn = dot(wi, highlight_normal);
                    float wodotn = dot(wo, highlight_normal);
                    //float A = m_sigma_s/m_sigma_t * (widotn*wodotn)/(widotn + wodotn);
                    widotn = (widotn < 0.f) ? 0.f : widotn;   
                    wodotn = (wodotn < 0.f) ? 0.f : wodotn;   
                    float A = 0.f;
                    if(widotn > 0.f && wodotn > 0.f){
                        A = 1.f / (4.0 * M_PI) * (widotn*wodotn)/(widotn + wodotn); //sigmas are "unimportant"
                        //TODO(Peter): Explain from where the 1/4*PI factor comes from
                    }
                    float l = 2.f;
                    //TODO(Peter): Implement As, -- smoothes the dissapeares of the
                    // higlight near the ends. Described in (9)
                    reflection = 2.f*l*m_umax*fc*Gu*A/m_delta_x;
                }
            }

            return reflection;
        }
        
        //can go
        float evalStapleSpecular(Vector wi, Vector wo, PatternData data, Intersection its) const {
            // Half-vector, for some reason it seems to already be in the
            // correct coordinate frame... 
            if(!data.warp_above){
                //float tmp1 = H.x;
                float tmp2 = wi.x;
                float tmp3 = wo.x;
                //H.x = H.y; H.y = tmp1;
                wi.x = -wi.y; wi.y = tmp2;
                wo.x = -wo.y; wo.y = tmp3;
            }
            Vector H = normalize(wi + wo);

            float u = data.u;
            float x = data.x;
            float D;
            {
                float a = H.y*sinf(u) + H.z*cosf(u);
                D = (H.y*cosf(u)-H.z*sinf(u))/(sqrtf(H.x*H.x + a*a)) / tanf(m_psi);
            }
            float reflection = 0.f;
            
            float specular_v = atan2f(-H.y*sinf(u) - H.z*cosf(u), H.x) + acosf(D); //Plus eller minus i sista termen.
            //TODO(Vidar): Clamp specular_v, do we need it?
            // Make normal for highlights, uses u and specular_v
            Vector highlight_normal = normalize(Vector(sinf(specular_v), sinf(u)*cosf(specular_v),
                cosf(u)*cosf(specular_v)));

            if (fabsf(specular_v) < M_PI_2 /*&& fabsf(D) < 1.f*/) {
                //we have specular reflection
                //get specular_x, using irawans transformation.
                float specular_x = specular_v/M_PI_2;
                // our transformation
                //float specular_x = sinf(specular_v);

                //Clamp specular_x
                specular_x = specular_x < 1.f - m_delta_x ? specular_x :
                    1.f - m_delta_x;
                specular_x = specular_x > -1.f + m_delta_x ? specular_x :
                    -1.f + m_delta_x;

                if (fabsf(specular_x - x) < m_delta_x) { //this takes the role of xi in the irawan paper.
                    // --- Set Gv
                    float a = 1.f; //radius of yarn
                    float R = 1.f/(sin(m_umax)); //radius of curvature
                    float Gv = a*(R + a*cosf(specular_v))/((wi + wo).length() * dot(highlight_normal,H) * fabsf(sinf(m_psi)));
                    // --- Set fc
                    float cos_x = -dot(wi, wo);
                    float fc = m_alpha + vonMises(cos_x, m_beta);
                    // --- Set A
                    float widotn = dot(wi, highlight_normal);
                    float wodotn = dot(wo, highlight_normal);
                    //float A = m_sigma_s/m_sigma_t * (widotn*wodotn)/(widotn + wodotn);
                    widotn = (widotn < 0.f) ? 0.f : widotn;   
                    wodotn = (wodotn < 0.f) ? 0.f : wodotn;   
                    //TODO(Vidar): This is where we get the NAN
                    float A = 0.f; //sigmas are "unimportant"
                    if(widotn > 0.f && wodotn > 0.f){
                        A = 1.f / (4.0 * M_PI) * (widotn*wodotn)/(widotn + wodotn); //sigmas are "unimportant"
                        //TODO(Peter): Explain from where the 1/4*PI factor comes from
                    }
                    float w = 2.f;
                    reflection = 2.f*w*m_umax*fc*Gv*A/m_delta_x;
                }
            }
#ifdef DO_DEBUG
            FILE *fp = fopen("/tmp/coordinate_out.txt","wt");
            if(fp){
                Frame frame = its.shFrame;
                Point p = its.p;
                Vector e1(1.f,0.f,0.f); e1 = frame.toWorld(e1);
                Vector e2(0.f,1.f,0.f); e2 = frame.toWorld(e2);
                Vector e3(0.f,0.f,1.f); e3 = frame.toWorld(e3);
                wi = frame.toWorld(wi);
                wo = frame.toWorld(wo);
                H  = frame.toWorld(H);
                highlight_normal  = frame.toWorld(highlight_normal);
                //normal  = frame.toWorld(normal);
                //fprintf(fp,"%d\n", ret_data.warp_above);
                fprintf(fp,"%f %f %f \n", p.x, p.y, p.z);
                fprintf(fp,"%f %f %f \n", e1.x, e1.y, e1.z);
                fprintf(fp,"%f %f %f \n", e2.x, e2.y, e2.z);
                fprintf(fp,"%f %f %f \n", e3.x, e3.y, e3.z);
                //fprintf(fp,"%f %f %f \n", normal.x, normal.y, normal.z);
                fprintf(fp,"%f %f %f \n", wi.x, wi.y, wi.z);
                fprintf(fp,"%f %f %f \n", wo.x, wo.y, wo.z);
                fprintf(fp,"%f %f %f \n",  H.x,  H.y,  H.z);
                fprintf(fp,"%f %f %f \n",  highlight_normal.x,  highlight_normal.y,  highlight_normal.z);
                fclose(fp);
            }
#endif
            return reflection;
        }
	
        //TODO(Peter): EXPLAIN! taken from irawan.cpp
        // von Mises Distribution
        Float vonMises(Float cos_x, Float b) const {
            // assumes a = 0, b > 0 is a concentration parameter.

            Float I0, absB = std::abs(b);
            if (std::abs(b) <= 3.75f) {
                Float t = absB / 3.75f;
                t = t * t;
                I0 = 1.0f + t*(3.5156229f + t*(3.0899424f + t*(1.2067492f
                                + t*(0.2659732f + t*(0.0360768f + t*0.0045813f)))));
            } else {
                Float t = 3.75f / absB;
                I0 = math::fastexp(absB) / std::sqrt(absB) * (0.39894228f + t*(0.01328592f
                            + t*(0.00225319f + t*(-0.00157565f + t*(0.00916281f + t*(-0.02057706f
                                            + t*(0.02635537f + t*(-0.01647633f + t*0.00392377f))))))));
            }

            return math::fastexp(b * cos_x) / (2 * M_PI * I0);
        }







        Spectrum eval(const BSDFSamplingRecord &bRec, EMeasure measure) const {
            if (!(bRec.typeMask & EDiffuseReflection) || measure != ESolidAngle
                    || Frame::cosTheta(bRec.wi) <= 0
                    || Frame::cosTheta(bRec.wo) <= 0)
                return Spectrum(0.0f);


            //Convert coordinates to correct format for our code...
            wcIntersectionData intersection_data;

            //get wi and wo in local shading corrds, plain uv.
            //from mitsuba api:
            //  Vector mitsuba::BSDFSamplingRecord::wi
            //  Normalized incident direction in local coordinates.
            intersection_data.wi_x = bRec.wi.x
            intersection_data.wi_y = bRec.wi.y
            intersection_data.wi_z = bRec.wi.z
                
            intersection_data.wo_x = bRec.wo.x
            intersection_data.wo_y = bRec.wo.y
            intersection_data.wo_z = bRec.wo.z
            
            intersection_data.uv_x = bRec.its.uv.x;
            intersection_data.uv_y = uv.y;

            sc - shade context
            Point3 uv = sc.UVW(1);

            p - unit vector along ray.
            d - vector to light

            p = sc.VectorFrom(p,REF_WORLD);
            // Transform the vector from the specified space to internal camera space.
            // p vector to transform
            // REF_WORLD The space to transform the vector from.
            // So, it transforms p from world space to shading space.
            
            d = sc.VectorFrom(d,REF_WORLD);

            // UVW derivatives
            Point3 dpdUVW[3];
            sc.DPdUVW(dpdUVW,1);

            //TODO(Vidar): I'm not certain that we want these dot products...
            Point3 n_vec = sc.Normal().Normalize();
            Point3 u_vec = dpdUVW[0].Normalize();
            Point3 v_vec = dpdUVW[1].Normalize();
            
            intersection_data.wi_x = ...;


            //perturb sampling record...
            //intensity variation...

            // Perturb the sampling record, in turn normal to match our thread normals
            PatternData pattern_data = getPatternData(bRec.its);
            Intersection perturbed(bRec.its);
            perturbed.shFrame = pattern_data.frame;

            Vector perturbed_wo = perturbed.toLocal(bRec.its.toWorld(bRec.wo));
            float diffuse_mask = 1.f;
            //Diffuse will be black if the perturbed direction
            //lies below the surface
            if(Frame::cosTheta(bRec.wo) * Frame::cosTheta(perturbed_wo) <= 0){
                diffuse_mask = 0.f;
            }

            //Get intensity variation
            float intensity_variation = 1.0f;
            if (m_intensity_fineness > 0.0f) {
                intensity_variation = intensityVariation(pattern_data);
            }
            
            Spectrum specular(m_specular_strength*intensity_variation
                    * m_specular_normalization
                    * specularReflectionPattern(bRec.wi, bRec.wo,
                        pattern_data,bRec.its));
            return m_reflectance->eval(bRec.its) * diffuse_mask * 
                pattern_data.color*(1.f - m_specular_strength) *
                (INV_PI * Frame::cosTheta(perturbed_wo)) +
                m_specular_strength*specular*Frame::cosTheta(bRec.wo);
        }

        Float pdf(const BSDFSamplingRecord &bRec, EMeasure measure) const {
            if (!(bRec.typeMask & EDiffuseReflection) || measure != ESolidAngle
                    || Frame::cosTheta(bRec.wi) <= 0
                    || Frame::cosTheta(bRec.wo) <= 0)
                return 0.0f;

            const Intersection& its = bRec.its;
            PatternData pattern_data = getPatternData(its);
            Intersection perturbed(its);
            perturbed.shFrame = pattern_data.frame;

            return warp::squareToCosineHemispherePdf(perturbed.toLocal(
                        its.toWorld(bRec.wo)));

        }

        Spectrum sample(BSDFSamplingRecord &bRec, const Point2 &sample) const {
            if (!(bRec.typeMask & EDiffuseReflection)
                    || Frame::cosTheta(bRec.wi) <= 0) return Spectrum(0.0f);
            const Intersection& its = bRec.its;
            Intersection perturbed(its);
            PatternData pattern_data = getPatternData(its);
            perturbed.shFrame = pattern_data.frame;
            bRec.wi = perturbed.toLocal(its.toWorld(bRec.wi));

            bRec.wo = warp::squareToCosineHemisphere(sample);
            Vector perturbed_wo = perturbed.toLocal(its.toWorld(bRec.wo));

            bRec.sampledComponent = 0;
            bRec.sampledType = EDiffuseReflection;
            bRec.eta = 1.f;
            float diffuse_mask = 0.f;
            if (Frame::cosTheta(perturbed_wo)
                    * Frame::cosTheta(bRec.wo) > 0){
                //We sample based on bRec.wo, take account of this
                diffuse_mask = Frame::cosTheta(perturbed_wo)/
                    Frame::cosTheta(bRec.wo);
            }
            Spectrum specular(m_specular_strength*specularReflectionPattern(
                        bRec.wi, bRec.wo, pattern_data,bRec.its));
            return m_reflectance->eval(bRec.its) * diffuse_mask *
                pattern_data.color*(1.f - m_specular_strength)
                + m_specular_strength*specular*m_specular_normalization;// *
        }

        Spectrum sample(BSDFSamplingRecord &bRec, Float &pdf, const Point2 &sample) const {
            if (!(bRec.typeMask & EDiffuseReflection) || Frame::cosTheta(bRec.wi) <= 0)
                return Spectrum(0.0f);

            const Intersection& its = bRec.its;
            Intersection perturbed(its);
            PatternData pattern_data = getPatternData(its);
            perturbed.shFrame = pattern_data.frame;
            bRec.wi = perturbed.toLocal(its.toWorld(bRec.wi));

            bRec.wo = warp::squareToCosineHemisphere(sample);
            Vector perturbed_wo = perturbed.toLocal(its.toWorld(bRec.wo));
            pdf = warp::squareToCosineHemispherePdf(bRec.wo);

            bRec.sampledComponent = 0;
            bRec.sampledType = EDiffuseReflection;
            bRec.eta = 1.f;
            float diffuse_mask = 0.f;
            if (Frame::cosTheta(perturbed_wo)
                    * Frame::cosTheta(bRec.wo) > 0){
                //We sample based on bRec.wo, take account of this
                diffuse_mask = Frame::cosTheta(perturbed_wo)/
                    Frame::cosTheta(bRec.wo);
            }
            Spectrum specular(m_specular_strength*specularReflectionPattern(
                        bRec.wi, bRec.wo, pattern_data,bRec.its));
            return m_reflectance->eval(bRec.its) * diffuse_mask *
                pattern_data.color*(1.f - m_specular_strength)
                + m_specular_strength*specular*m_specular_normalization;// * 
        }

        void addChild(const std::string &name, ConfigurableObject *child) {
            BSDF::addChild(name, child);
        }

        void serialize(Stream *stream, InstanceManager *manager) const {
            BSDF::serialize(stream, manager);
            //TODO(Vidar): Serialize our parameters

            manager->serialize(stream, m_reflectance.get());
        }

        Float getRoughness(const Intersection &its, int component) const {
            return std::numeric_limits<Float>::infinity();
        }

        std::string toString() const {
            //TODO(Vidar): Add our parameters here...
            std::ostringstream oss;
            oss << "Cloth[" << endl
                << "  id = \"" << getID() << "\"," << endl
                << "  reflectance = " << indent(m_reflectance->toString()) << endl
                << "]";
            return oss.str();
        }

        Shader *createShader(Renderer *renderer) const;

        MTS_DECLARE_CLASS()
    private:
            ref<Texture> m_reflectance;
            PaletteEntry * m_pattern_entry;
            uint32_t m_pattern_height;
            uint32_t m_pattern_width;
            float m_uscale;
            float m_vscale;
            float m_umax;
            float m_psi;
            float m_alpha;
            float m_beta;
            float m_sigma_s;
            float m_sigma_t;
            float m_delta_x;
            float m_specular_strength;
            float m_intensity_fineness;
            float m_specular_normalization;
            wcWeaveParameters m_weave_parameters;
};

// ================ Hardware shader implementation ================

class SmoothDiffuseShader : public Shader {
    public:
        SmoothDiffuseShader(Renderer *renderer, const Texture *reflectance)
            : Shader(renderer, EBSDFShader), m_reflectance(reflectance) {
                m_reflectanceShader = renderer->registerShaderForResource(m_reflectance.get());
            }

        bool isComplete() const {
            return m_reflectanceShader.get() != NULL;
        }

        void cleanup(Renderer *renderer) {
            renderer->unregisterShaderForResource(m_reflectance.get());
        }

        void putDependencies(std::vector<Shader *> &deps) {
            deps.push_back(m_reflectanceShader.get());
        }

        void generateCode(std::ostringstream &oss,
                const std::string &evalName,
                const std::vector<std::string> &depNames) const {
            oss << "vec3 " << evalName << "(vec2 uv, vec3 wi, vec3 wo) {" << endl
                << "    if (cosTheta(wi) < 0.0 || cosTheta(wo) < 0.0)" << endl
                << "    	return vec3(0.0);" << endl
                << "    return " << depNames[0] << "(uv) * inv_pi * cosTheta(wo);" << endl
                << "}" << endl
                << endl
                << "vec3 " << evalName << "_diffuse(vec2 uv, vec3 wi, vec3 wo) {" << endl
                << "    return " << evalName << "(uv, wi, wo);" << endl
                << "}" << endl;
        }

        MTS_DECLARE_CLASS()
    private:
            ref<const Texture> m_reflectance;
            ref<Shader> m_reflectanceShader;
};

Shader *Cloth::createShader(Renderer *renderer) const {
    return new SmoothDiffuseShader(renderer, m_reflectance.get());
}

    MTS_IMPLEMENT_CLASS(SmoothDiffuseShader, false, Shader)
MTS_IMPLEMENT_CLASS_S(Cloth, false, BSDF)
    MTS_EXPORT_PLUGIN(Cloth, "Smooth diffuse BRDF")
    MTS_NAMESPACE_END